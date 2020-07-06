#include <atomic>
#include <memory>

#include "common/common/logger.h"

using spdlog::level::level_enum;

namespace Envoy {

/**
 * FancyLogger implementation: a logger with fine-grained log control.
 */

/**
 * If the sink is initialized.
 */
int kSinkInit = 0;

/**
 * Lock for the following global map (not for the corresponding loggers).
 */
absl::Mutex fancy_log_lock__;

spdlog::level::level_enum kFancyDefaultLevel = spdlog::level::info;


/**
 * Global hash map <unit, log info>, where unit can be file, function or line.
 */
FancyMapPtr getFancyLogMap() ABSL_EXCLUSIVE_LOCKS_REQUIRED(fancy_log_lock__) {
  static FancyMapPtr fancy_log_map = std::make_shared<FancyMap>();
  return fancy_log_map;
}

// /**
//  * Implementation of BasicLockable, to avoid dependency
//  * problem of thread.h.
//  */
// class FancyBasicLockable : public Thread::BasicLockable {
// public:
//   // BasicLockable
//   void lock() ABSL_EXCLUSIVE_LOCK_FUNCTION() override { mutex_.Lock(); }
//   bool tryLock() ABSL_EXCLUSIVE_TRYLOCK_FUNCTION(true) override { return mutex_.TryLock(); }
//   void unlock() ABSL_UNLOCK_FUNCTION() override { mutex_.Unlock(); }

// private:
//   absl::Mutex mutex_;
// };

namespace {

// spdlog::sink_ptr getSink() {
//   static spdlog::sink_ptr sink = Logger::DelegatingLogSink::init();
//   return sink;
// }

// /**
//  * Initialize sink for the initialization of loggers, once and for all.
//  */
// void initSink() {
//   spdlog::sink_ptr sink = Logger::Registry::getSink();
//   Logger::DelegatingLogSinkSharedPtr sp = std::static_pointer_cast<Logger::DelegatingLogSink>(sink);
//   if (!sp->hasLock()) {
//     static FancyBasicLockable tlock;
//     sp->setLock(tlock);
//     sp->set_should_escape(false); // unsure
//     printf("sink lock: %s\n", sp->hasLock() ? "true" : "false");
//   }
// }

/**
 * Create a logger and add it to map.
 */
spdlog::logger* createLogger(std::string key, int level = -1)
    ABSL_EXCLUSIVE_LOCKS_REQUIRED(fancy_log_lock__) {
  SpdLoggerPtr new_logger = std::make_shared<spdlog::logger>(key, Logger::Registry::getSink());
  level_enum lv = Logger::Context::fancy_default_level_;
  if (level > -1) {
    lv = static_cast<level_enum>(level);
  }
  new_logger->set_level(lv);
  new_logger->set_pattern(Logger::Context::fancy_log_format_);
  new_logger->flush_on(level_enum::critical);
  getFancyLogMap()->insert(std::make_pair(key, new_logger));
  return new_logger.get();
}

} // namespace

/**
 * Initialize Fancy Logger and register it in global map if not done.
 */
void initFancyLogger(std::string key, std::atomic<spdlog::logger*>& logger) {
  absl::WriterMutexLock l(&fancy_log_lock__);
  // if (!kSinkInit) {
  //   initSink();
  // }
  auto it = getFancyLogMap()->find(key);
  spdlog::logger* target;
  if (it == getFancyLogMap()->end()) {
    target = createLogger(key);
  } else {
    target = it->second.get();
  }
  logger.store(target);
}

/**
 * Set log level.
 */
void setFancyLogger(std::string key, level_enum log_level) {
  absl::WriterMutexLock l(&fancy_log_lock__);
  auto it = getFancyLogMap()->find(key);
  if (it != getFancyLogMap()->end()) {
    it->second->set_level(log_level);
  } else {
    createLogger(key, static_cast<int>(log_level));
  }
}

} // namespace Envoy
