#include "beringei/lib/GorillaStatsManager.h"

#include "beringei/client/WriteClient.h"
#include "beringei/lib/Timer.h"

DEFINE_int32(
    gorilla_retry_queue_capacity,
    10000,
    "The number of data points that will fit in the retry queue");
DEFINE_int32(
    gorilla_retry_delay_secs,
    55,
    "Retry delay for failed sends. Keeping this under one minute will "
    "still allow data points to arrive in the correct order (assuming "
    "one minute data)");
DEFINE_int32(
    gorilla_write_retry_threads,
    4,
    "Number of threads for retrying failed writes");

namespace facebook {
namespace gorilla {

const static std::string kPutKey = "gorilla_client.put.";
const static std::string kUsPerPut = "gorilla_client.us_per_put.";
const static std::string kPutDroppedKey = "gorilla_client.put_dropped.";
const static std::string kPutRetryKey = "gorilla_client.put_retry.";
const static std::string kRetryQueueWriteFailures =
    "gorilla_client.retry_queue_write_failures";
const static std::string kRetryQueueSizeKey = "gorilla_client.retry_queue_size";
const static int kRetryThresholdSecs = 30;
// The vectors can be a lot smaller in the retry queue.
const static int kRetryQueueCapacitySizeRatio = 100;
const static int kMinQueueSize = 10;

WriteClient::WriteClient(
    std::unique_ptr<BeringeiNetworkClient> networkClient,
    size_t queueCapacity,
    size_t queueSize)
    : queue(queueCapacity, queueSize),
      client(std::move(networkClient)),
      retryQueue_(std::max(
          (int)FLAGS_gorilla_retry_queue_capacity /
              kRetryQueueCapacitySizeRatio,
          kMinQueueSize)),
      counterUsPerPut_(kUsPerPut + client->getServiceName()),
      counterPutKey_(kPutKey + client->getServiceName()),
      counterPutRetryKey_(kPutRetryKey + client->getServiceName()),
      counterPutDroppedKey_(kPutDroppedKey + client->getServiceName()) {
  GorillaStatsManager::addStatExportType(kRetryQueueSizeKey, AVG);
  GorillaStatsManager::addStatValue(kRetryQueueSizeKey, 0);
  GorillaStatsManager::addStatExportType(kRetryQueueWriteFailures, SUM);
  GorillaStatsManager::addStatExportType(counterPutRetryKey_, SUM);
  GorillaStatsManager::addStatExportType(counterPutRetryKey_, COUNT);
}

void WriteClient::retry(std::vector<DataPoint> dropped) {
  // Retry and send the failed data points in another thread
  // after a delay to allow the server to come back up if it's
  // down.
  size_t droppedCount = dropped.size();
  RetryOperation op;
  op.dataPoints = std::move(dropped);
  op.retryTimeSecs = time(nullptr) + FLAGS_gorilla_retry_delay_secs;
  if (numRetryQueuedDataPoints_ + droppedCount >=
          FLAGS_gorilla_retry_queue_capacity ||
      !retryQueue_.write(std::move(op))) {
    logDroppedDataPoints(droppedCount, "retry queue is full");
    GorillaStatsManager::addStatValue(kRetryQueueWriteFailures);
  } else {
    numRetryQueuedDataPoints_ += droppedCount;
    GorillaStatsManager::addStatValue(counterPutRetryKey_, droppedCount);
    GorillaStatsManager::addStatValue(
        kRetryQueueSizeKey, numRetryQueuedDataPoints_);
  }
}

void WriteClient::retryThread() {
  while (true) {
    try {
      BeringeiNetworkClient::PutRequestMap requestMap;
      RetryOperation op;
      retryQueue_.blockingRead(op);
      numRetryQueuedDataPoints_ -= op.dataPoints.size();
      GorillaStatsManager::addStatValue(
          kRetryQueueSizeKey, numRetryQueuedDataPoints_);

      if (op.dataPoints.empty()) {
        LOG(INFO) << "Shutting down retry thread";
        break;
      }

      if (op.retryTimeSecs < time(nullptr) - kRetryThresholdSecs) {
        logDroppedDataPoints(op.dataPoints.size(), "data points are too old");
        continue;
      }

      if (op.retryTimeSecs > time(nullptr)) {
        // Sleeping is fine because it's a FIFO queue with a constant delay.
        sleep(op.retryTimeSecs - time(nullptr));
      }

      // Build the request.
      uint32_t totalDropped = 0;
      for (auto& dp : op.dataPoints) {
        bool dropped = false;
        client->addDataPointToRequest(dp, requestMap, dropped);
        if (dropped) {
          totalDropped++;
        }
      }

      // Send the data points.
      std::vector<DataPoint> dropped =
          putWithStats(op.dataPoints.size() - totalDropped, requestMap);
      totalDropped += dropped.size();
      if (totalDropped > 0) {
        logDroppedDataPoints(totalDropped, "retry send failed");
      }

    } catch (std::exception& e) {
      LOG(ERROR) << e.what();
    }
  }
}

std::vector<DataPoint> WriteClient::putWithStats(
    int points,
    BeringeiNetworkClient::PutRequestMap& requestMap) {
  Timer timer(true);
  std::vector<DataPoint> dropped = client->performPut(requestMap);
  GorillaStatsManager::addStatValue(counterUsPerPut_, timer.get());
  GorillaStatsManager::addStatValue(counterPutKey_, points - dropped.size());
  return dropped;
}

void WriteClient::logDroppedDataPoints(
    uint32_t dropped,
    const std::string& msg) {
  LOG(WARNING) << "Dropping " << dropped << " data points for service "
               << client->getServiceName() << " because " << msg;
  GorillaStatsManager::addStatValue(counterPutDroppedKey_, dropped);
}

void WriteClient::start() {
  for (int i = 0; i < FLAGS_gorilla_write_retry_threads; i++) {
    retryWriters_.emplace_back(&WriteClient::retryThread, this);
  }
}

void WriteClient::stop() {
  for (auto& thread : retryWriters_) {
    RetryOperation op;
    op.retryTimeSecs = 0;
    // Empty data points vector will stop the thread.
    retryQueue_.write(std::move(op));
  }

  for (auto& thread : retryWriters_) {
    thread.join();
  }
  retryWriters_.clear();
}

size_t WriteClient::getNumShards() const {
  return client->getNumShards();
}

} // namespace gorilla
} // namespace facebook