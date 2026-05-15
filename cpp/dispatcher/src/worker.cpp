#include "worker.hpp"

#include <chrono>
#include <exception>
#include <mutex>
#include <string>
#include <thread>

#include "logging.hpp"

namespace nlr {

Worker::Worker(const Config& cfg, Destination destination, const std::string& worker_id)
    : cfg_(cfg),
      destination_(std::move(destination)),
      worker_id_(worker_id)
{
    db_      = std::make_unique<Db>(cfg_.database_url);
    handler_ = make_handler(destination_.kind);
    if (!handler_) {
        LOG_WARN("worker.unsupported_kind",
            "destination_id", std::to_string(destination_.id),
            "destination",    destination_.name,
            "kind",           destination_.kind);
    }
}

Worker::~Worker() {
    stop();
    join();
}

void Worker::start() {
    thread_ = std::thread([this] { run_(); });
}

void Worker::stop() noexcept {
    stop_requested_.store(true, std::memory_order_relaxed);
}

void Worker::join() {
    if (thread_.joinable()) thread_.join();
}

void Worker::update_destination(Destination updated) {
    std::lock_guard<std::mutex> lock(destination_m_);
    destination_ = std::move(updated);
}

void Worker::run_() {
    LOG_INFO("worker.start",
        "destination_id", std::to_string(destination_.id),
        "destination",    destination_.name,
        "kind",           destination_.kind);

    while (!stop_requested_.load(std::memory_order_relaxed)) {
        if (!handler_) {
            // Unsupported kind: idle. Refresh might give us a handler later
            // if the operator changes the destination kind (rare).
            std::this_thread::sleep_for(std::chrono::milliseconds(cfg_.poll_interval_ms));
            continue;
        }

        // Snapshot the destination so an in-flight refresh on the server
        // thread doesn't race with our dispatch call.
        Destination dst;
        {
            std::lock_guard<std::mutex> lock(destination_m_);
            dst = destination_;
        }

        std::vector<Assignment> claimed;
        try {
            claimed = db_->claim_pending_for_destination(
                dst.id, cfg_.server_id, worker_id_,
                cfg_.lease_seconds, cfg_.batch_size);
        } catch (const std::exception& e) {
            LOG_ERROR("worker.claim_failed",
                "destination_id", std::to_string(dst.id),
                "error",          e.what());
            std::this_thread::sleep_for(std::chrono::milliseconds(cfg_.poll_interval_ms));
            continue;
        }

        if (claimed.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(cfg_.poll_interval_ms));
            continue;
        }

        LOG_DEBUG("worker.batch_claimed",
            "destination_id", std::to_string(dst.id),
            "count",          std::to_string(claimed.size()));

        for (const auto& a : claimed) {
            if (stop_requested_.load(std::memory_order_relaxed)) break;

            DispatchResult result = DispatchResult::permanent("uninitialized");
            try {
                result = handler_->dispatch(a, dst);
            } catch (const std::exception& e) {
                result = DispatchResult::transient(
                    std::string{"uncaught: "} + e.what());
            } catch (...) {
                result = DispatchResult::transient("uncaught: unknown");
            }

            try {
                switch (result.status) {
                    case DispatchResult::Status::Success: {
                        db_->mark_dispatched(a.id, result.response_detail_json);
                        LOG_INFO("worker.dispatched",
                            "assignment_id",  std::to_string(a.id),
                            "destination_id", std::to_string(dst.id),
                            "study_uid",      a.study_instance_uid,
                            "detail",         result.response_detail_json);
                        break;
                    }
                    case DispatchResult::Status::TransientFail: {
                        const bool permanent = db_->mark_failed_with_retry(
                            a.id, result.error_message,
                            result.response_detail_json, dst);
                        LOG_WARN(permanent ? "worker.failed_permanent" : "worker.failed_retry",
                            "assignment_id",  std::to_string(a.id),
                            "destination_id", std::to_string(dst.id),
                            "study_uid",      a.study_instance_uid,
                            "error",          result.error_message);
                        break;
                    }
                    case DispatchResult::Status::PermanentFail: {
                        db_->mark_failed_permanent(a.id, result.error_message,
                                                    result.response_detail_json);
                        LOG_ERROR("worker.failed_permanent",
                            "assignment_id",  std::to_string(a.id),
                            "destination_id", std::to_string(dst.id),
                            "study_uid",      a.study_instance_uid,
                            "error",          result.error_message);
                        break;
                    }
                }
            } catch (const std::exception& e) {
                // Failure to update the DB after a dispatch is bad — the
                // assignment is stuck in 'dispatching' until its lease
                // expires. Log loudly; the lease sweeper (M10) will
                // re-pickup.
                LOG_ERROR("worker.status_update_failed",
                    "assignment_id", std::to_string(a.id),
                    "error",         e.what());
            }
        }
    }

    LOG_INFO("worker.stop",
        "destination_id", std::to_string(destination_.id),
        "destination",    destination_.name);
}

}  // namespace nlr
