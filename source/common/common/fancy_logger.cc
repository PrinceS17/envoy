#include "common/common/fancy_logger.h"

#include <atomic>
#include <memory>

#include "common/common/logger.h"

using spdlog::level::level_enum;

namespace Envoy {

absl::Mutex FancyContext::fancy_log_lock_(absl::kConstInit);

FancyMapPtr FancyContext::getFancyLogMap() {
  static FancyMapPtr fancy_log_map = std::make_shared<FancyMap>();
  return fancy_log_map;
}

absl::Mutex* FancyContext::getFancyLogLock() { return &fancy_log_lock_; }

/**
 * Implements a lock from BasicLockable, to avoid dependency problem of thread.h.
 */
class FancyBasicLockable : public Thread::BasicLockable {
public:
  // BasicLockable
  void lock() ABSL_EXCLUSIVE_LOCK_FUNCTION() override { mutex_.Lock(); }
  bool tryLock() ABSL_EXCLUSIVE_TRYLOCK_FUNCTION(true) override { return mutex_.TryLock(); }
  void unlock() ABSL_UNLOCK_FUNCTION() override { mutex_.Unlock(); }

private:
  absl::Mutex mutex_;
};

void FancyContext::initSink() {
  spdlog::sink_ptr sink = Logger::Registry::getSink();
  Logger::DelegatingLogSinkSharedPtr sp = std::static_pointer_cast<Logger::DelegatingLogSink>(sink);
  if (!sp->hasLock()) {
    static FancyBasicLockable tlock;
    sp->setLock(tlock);
    sp->set_should_escape(false);
  }
}

spdlog::logger* FancyContext::createLogger(std::string key, int level)
    ABSL_EXCLUSIVE_LOCKS_REQUIRED(fancy_log_lock_) {
  SpdLoggerPtr new_logger = std::make_shared<spdlog::logger>(key, Logger::Registry::getSink());
  if (!Logger::Registry::getSink()->hasLock()) {
    initSink();
  }
  level_enum lv = Logger::Context::getFancyDefaultLevel();
  if (level > -1) {
    lv = static_cast<level_enum>(level);
  }
  new_logger->set_level(lv);
  new_logger->set_pattern(Logger::Context::getFancyLogFormat());
  new_logger->flush_on(level_enum::critical);
  getFancyLogMap()->insert(std::make_pair(key, new_logger));
  return new_logger.get();
}

void FancyContext::initFancyLogger(std::string key, std::atomic<spdlog::logger*>& logger) {
  absl::WriterMutexLock l(&FancyContext::fancy_log_lock_);
  auto it = getFancyLogMap()->find(key);
  spdlog::logger* target;
  if (it == getFancyLogMap()->end()) {
    target = createLogger(key);
  } else {
    target = it->second.get();
  }
  logger.store(target);
}

bool FancyContext::setFancyLogger(std::string key, level_enum log_level) {
  absl::ReaderMutexLock l(&FancyContext::fancy_log_lock_);
  auto it = getFancyLogMap()->find(key);
  if (it != getFancyLogMap()->end()) {
    it->second->set_level(log_level);
    return true;
  }
  return false;
}

} // namespace Envoy
