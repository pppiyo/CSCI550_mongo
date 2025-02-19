/**
 *    Copyright (C) 2023-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "pinned_connection_task_executor.h"
#include "mongo/executor/network_interface.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/util/scoped_unlock.h"

namespace mongo::executor {
/**
 * Used as the state for callbacks _only_ for RPCs scheduled through this executor.
 */
class PinnedConnectionTaskExecutor::CallbackState : public TaskExecutor::CallbackState {
    CallbackState(const CallbackState&) = delete;
    CallbackState& operator=(const CallbackState&) = delete;

public:
    static std::shared_ptr<CallbackState> make(const RemoteCommandOnAnyCallbackFn& cb,
                                               const BatonHandle& baton) {
        return std::make_shared<CallbackState>(cb, baton);
    }

    /**
     * Do not call directly. Use make.
     */
    CallbackState(const RemoteCommandOnAnyCallbackFn& cb, const BatonHandle& baton)
        : callback(cb), baton(baton) {}

    virtual ~CallbackState() = default;

    bool isCanceled() const override {
        MONGO_UNREACHABLE;
    }

    void cancel() override {
        MONGO_UNREACHABLE;
    }

    void waitForCompletion() override {
        MONGO_UNREACHABLE;
    }

    // Run callback with a CallbackCanceled error.
    static void runCallbackCanceled(stdx::unique_lock<Latch>& lk,
                                    RequestAndCallback rcb,
                                    TaskExecutor* exec) {
        CallbackHandle cbHandle;
        setCallbackForHandle(&cbHandle, rcb.second);
        auto errorResponse = RemoteCommandOnAnyResponse(boost::none, kCallbackCanceledErrorStatus);
        TaskExecutor::RemoteCommandOnAnyCallbackFn callback;
        using std::swap;
        swap(rcb.second->callback, callback);
        ScopedUnlock guard(lk);
        callback({exec, cbHandle, rcb.first, errorResponse});
    }

    // Run callback with the provided result.
    static void runCallbackFinished(stdx::unique_lock<Latch>& lk,
                                    RequestAndCallback rcb,
                                    TaskExecutor* exec,
                                    const StatusWith<RemoteCommandResponse>& result,
                                    boost::optional<HostAndPort> targetUsed) {
        // Convert the result into a RemoteCommandResponse unconditionally.
        RemoteCommandResponse asRcr =
            result.isOK() ? result.getValue() : RemoteCommandResponse(result.getStatus());
        // Convert the response into an OnAnyResponse using the provided target.
        RemoteCommandOnAnyResponse asOnAnyRcr(targetUsed, asRcr);
        CallbackHandle cbHandle;
        setCallbackForHandle(&cbHandle, rcb.second);
        TaskExecutor::RemoteCommandOnAnyCallbackFn callback;
        using std::swap;
        swap(rcb.second->callback, callback);
        ScopedUnlock guard(lk);
        callback({exec, cbHandle, rcb.first, asOnAnyRcr});
    }

    // All fields except for "canceled" are guarded by the owning task executor's _mutex.
    enum class State { kWaiting, kRunning, kDone, kCanceled };

    RemoteCommandOnAnyCallbackFn callback;
    boost::optional<stdx::condition_variable> finishedCondition;
    State state{State::kWaiting};
    bool isNetworkOperation = true;
    bool startedNetworking = false;
    BatonHandle baton;
};

PinnedConnectionTaskExecutor::PinnedConnectionTaskExecutor(
    const std::shared_ptr<TaskExecutor>& executor, NetworkInterface* net)
    : _executor(executor), _net(net), _cancellationExecutor(executor) {}

PinnedConnectionTaskExecutor::~PinnedConnectionTaskExecutor() {
    shutdown();
    join();
}

Date_t PinnedConnectionTaskExecutor::now() {
    return _executor->now();
}

StatusWith<TaskExecutor::EventHandle> PinnedConnectionTaskExecutor::makeEvent() {
    return _executor->makeEvent();
}

void PinnedConnectionTaskExecutor::signalEvent(const EventHandle& event) {
    return _executor->signalEvent(event);
}

StatusWith<TaskExecutor::CallbackHandle> PinnedConnectionTaskExecutor::onEvent(
    const EventHandle& event, CallbackFn&& work) {
    return _executor->onEvent(event, std::move(work));
}

void PinnedConnectionTaskExecutor::waitForEvent(const EventHandle& event) {
    _executor->waitForEvent(event);
}

StatusWith<stdx::cv_status> PinnedConnectionTaskExecutor::waitForEvent(OperationContext* opCtx,
                                                                       const EventHandle& event,
                                                                       Date_t deadline) {
    return _executor->waitForEvent(opCtx, event, deadline);
}

StatusWith<TaskExecutor::CallbackHandle> PinnedConnectionTaskExecutor::scheduleWork(
    CallbackFn&& work) {
    return _executor->scheduleWork(std::move(work));
}

StatusWith<TaskExecutor::CallbackHandle> PinnedConnectionTaskExecutor::scheduleWorkAt(
    Date_t when, CallbackFn&& work) {
    return _executor->scheduleWorkAt(when, std::move(work));
}

StatusWith<TaskExecutor::CallbackHandle> PinnedConnectionTaskExecutor::scheduleRemoteCommandOnAny(
    const RemoteCommandRequestOnAny& requestOnAny,
    const RemoteCommandOnAnyCallbackFn& cb,
    const BatonHandle& baton) {

    stdx::unique_lock<Latch> lk{_mutex};
    if (_state != State::running) {
        return {ErrorCodes::ShutdownInProgress, "Shutdown in progress"};
    }
    invariant(requestOnAny.target.size() == 1,
              "RPCs scheduled through PinnedConnectionTaskExecutor can only target a single host.");
    RemoteCommandRequest req = RemoteCommandRequest(requestOnAny, 0);
    auto state = PinnedConnectionTaskExecutor::CallbackState::make(cb, baton);
    _requestQueue.push_back({req, state});

    CallbackHandle cbHandle;
    setCallbackForHandle(&cbHandle, state);

    if (!_isDoingNetworking) {
        _doNetworking(std::move(lk));
    }

    return cbHandle;
}

void PinnedConnectionTaskExecutor::_cancel(WithLock, CallbackState* cbState) {
    switch (cbState->state) {
        case CallbackState::State::kWaiting:
            // Just set the state to canceled. The callback will be run with an
            // error status once it reaches the front of the queue.
            cbState->state = CallbackState::State::kCanceled;
            break;
        case CallbackState::State::kRunning: {
            // Cancel the ongoing operation.
            cbState->state = CallbackState::State::kCanceled;
            if (_stream) {
                auto client = _stream->getClient();
                client->cancel(cbState->baton);
            }
            break;
        }
        case CallbackState::State::kCanceled:
            [[fallthrough]];
        case CallbackState::State::kDone:
            // Nothing to do.
            break;
    }
}

void PinnedConnectionTaskExecutor::cancel(const CallbackHandle& cbHandle) {
    auto cbState =
        dynamic_cast<PinnedConnectionTaskExecutor::CallbackState*>(getCallbackFromHandle(cbHandle));
    if (!cbState) {
        // Defer to underlying for non-RPC.
        _executor->cancel(cbHandle);
        return;
    }
    stdx::lock_guard lk(_mutex);
    return _cancel(std::move(lk), cbState);
}

ExecutorFuture<void> PinnedConnectionTaskExecutor::_ensureStream(
    WithLock, HostAndPort target, Milliseconds timeout, transport::ConnectSSLMode sslMode) {
    if (!_stream) {
        auto streamFuture = _net->leaseStream(target, sslMode, timeout);
        // If the stream is ready, send the RPC immediately by continuing inline.
        if (streamFuture.isReady()) {
            auto stream = std::move(streamFuture).getNoThrow();
            if (!stream.isOK()) {
                // Propogate  the error down the future chain.
                return ExecutorFuture<void>(*_executor, stream.getStatus());
            }
            _stream = std::move(stream.getValue());
            return ExecutorFuture<void>(*_executor);
        }
        // Otherwise continue on the networking reactor once the stream is ready.
        return std::move(streamFuture)
            .thenRunOn(*_executor)
            .then([this](std::unique_ptr<NetworkInterface::LeasedStream> stream) {
                stdx::lock_guard lk{_mutex};
                _stream = std::move(stream);
            });
    }

    auto remote = _stream->getClient()->remote();
    using namespace fmt::literals;
    invariant(
        target == remote,
        "Attempted to schedule RPC to {} on TaskExecutor that had pinned connection to {}"_format(
            target, remote));
    return ExecutorFuture<void>(*_executor);
}

Future<executor::RemoteCommandResponse> PinnedConnectionTaskExecutor::_runSingleCommand(
    RemoteCommandRequest command, std::shared_ptr<CallbackState> cbState) {
    stdx::lock_guard lk{_mutex};
    if (auto& state = cbState->state; MONGO_unlikely(state == CallbackState::State::kCanceled)) {
        // It's possible this callback was canceled after it was moved
        // out of the queue, but before we actually started work on the client.
        // In that case, don't run it.
        return kCallbackCanceledErrorStatus;
    }
    auto client = _stream->getClient();
    cbState->startedNetworking = true;
    return client->runCommandRequest(command, cbState->baton);
}

boost::optional<PinnedConnectionTaskExecutor::RequestAndCallback>
PinnedConnectionTaskExecutor::_getFirstUncanceledRequest(stdx::unique_lock<Latch>& lk) {
    while (!_requestQueue.empty()) {
        auto req = std::move(_requestQueue.front());
        _requestQueue.pop_front();
        if (req.second->state == CallbackState::State::kCanceled) {
            CallbackState::runCallbackCanceled(lk, req, this);
        } else {
            return req;
        }
    }
    return boost::none;
}

void PinnedConnectionTaskExecutor::_doNetworking(stdx::unique_lock<Latch>&& lk) {
    _isDoingNetworking = true;
    // Find the first non-canceled request.
    boost::optional<RequestAndCallback> maybeReqToRun = _getFirstUncanceledRequest(lk);
    if (!maybeReqToRun) {
        // No non-canceled requests. Stop doing networking.
        _isDoingNetworking = false;
        invariant(_requestQueue.empty());
        _requestQueueEmptyCV.notify_all();
        return;
    }
    auto req = *maybeReqToRun;
    // Set req state to running
    invariant(req.second->state == CallbackState::State::kWaiting);
    req.second->state = CallbackState::State::kRunning;
    auto streamFut = _ensureStream(lk, req.first.target, req.first.timeout, req.first.sslMode);
    // Stash the in-progress operation before releasing the lock so we can
    // access it if we're shutdown while it's in-progress.
    _inProgressRequest = req.second;
    lk.unlock();
    std::move(streamFut)
        .then([req, this]() { return _runSingleCommand(req.first, req.second); })
        .thenRunOn(makeGuaranteedExecutor(req.second->baton, _cancellationExecutor))
        .getAsync([req, this](StatusWith<RemoteCommandResponse> result) {
            stdx::unique_lock<Latch> lk{_mutex};
            _inProgressRequest.reset();
            // If we used the _stream, update it accordingly.
            if (req.second->startedNetworking) {
                if (auto status = result.getStatus(); status.isOK()) {
                    _stream->indicateUsed();
                    _stream->indicateSuccess();
                } else {
                    // We didn't get a response from the remote.
                    // We assume the stream is broken and therefore can do no more work. Notify the
                    // stream of the failure, destroy it, and shutdown.
                    _stream->indicateFailure(status);
                    _stream.reset();
                    _shutdown(lk);
                }
            }
            // Now run the completion callback for the command.
            if (auto& state = req.second->state;
                MONGO_unlikely(state == CallbackState::State::kCanceled)) {
                CallbackState::runCallbackCanceled(lk, req, this);
            } else {
                invariant(state == CallbackState::State::kRunning);
                // Three possibilities here: we either finished the RPC
                // successfully, got a local error from the stream after
                // attempting to start networking, or never were able to acquire a
                // stream. In any case, we first complete the current request
                // by invoking it's callback:
                state = CallbackState::State::kDone;
                // Get the target if we successfully acquired a stream.
                boost::optional<HostAndPort> target = boost::none;
                if (_stream) {
                    target = _stream->getClient()->remote();
                }
                CallbackState::runCallbackFinished(lk, req, this, result, target);
            }
            // If we weren't able to acquire a stream, shut-down.
            if (!_stream) {
                _shutdown(lk);
            }
            _isDoingNetworking = false;
            if (!_requestQueue.empty()) {
                return _doNetworking(std::move(lk));
            }
            _requestQueueEmptyCV.notify_all();
        });
}

void PinnedConnectionTaskExecutor::_shutdown(WithLock lk) {
    if (_state != State::running) {
        return;
    }
    _state = State::joinRequired;
    _executor->shutdown();
    for (auto&& [_, cbState] : _requestQueue) {
        _cancel(lk, cbState.get());
    }
    if (_isDoingNetworking && _inProgressRequest) {
        // Cancel the in-progress request that was already popped from the queue.
        _cancel(lk, _inProgressRequest.get());
    }
}

void PinnedConnectionTaskExecutor::shutdown() {
    stdx::lock_guard lk(_mutex);
    _shutdown(lk);
}

// May be called by any thread that wishes to wait until this executor is done shutting down.
// Any thread that calls this will block until no work remains scheduled but not completed
// on this executor. After join() completes, the state if this executor will be 'shutdownComplete'.
void PinnedConnectionTaskExecutor::join() {
    stdx::unique_lock lk(_mutex);
    if (_state == State::shutdownComplete) {
        return;
    }
    invariant(_state == State::joinRequired || _state == State::joining);
    _state = State::joining;

    _requestQueueEmptyCV.wait(lk,
                              [this]() { return _requestQueue.empty() && !_isDoingNetworking; });

    _executor->join();

    _state = State::shutdownComplete;
    return;
}

SharedSemiFuture<void> PinnedConnectionTaskExecutor::joinAsync() {
    MONGO_UNREACHABLE;
}

bool PinnedConnectionTaskExecutor::isShuttingDown() const {
    stdx::lock_guard lk(_mutex);
    return _state != State::running;
}


// Below are the portions of the TaskExecutor API that are illegal to use through
// PinnedCursorTaskExecutor and/or are unimplemented at this time.
void PinnedConnectionTaskExecutor::wait(const CallbackHandle& cbHandle,
                                        Interruptible* interruptible) {
    MONGO_UNREACHABLE;
}

StatusWith<TaskExecutor::CallbackHandle>
PinnedConnectionTaskExecutor::scheduleExhaustRemoteCommandOnAny(
    const RemoteCommandRequestOnAny& request,
    const RemoteCommandOnAnyCallbackFn& cb,
    const BatonHandle& baton) {
    MONGO_UNREACHABLE;
}

bool PinnedConnectionTaskExecutor::hasTasks() {
    stdx::lock_guard lk(_mutex);
    return (!_requestQueue.empty()) || _executor->hasTasks();
}

void PinnedConnectionTaskExecutor::startup() {
    MONGO_UNREACHABLE;
}

void PinnedConnectionTaskExecutor::appendDiagnosticBSON(mongo::BSONObjBuilder* builder) const {
    MONGO_UNREACHABLE;
}


void PinnedConnectionTaskExecutor::appendConnectionStats(ConnectionPoolStats* stats) const {
    MONGO_UNREACHABLE;
}

void PinnedConnectionTaskExecutor::dropConnections(const HostAndPort& hostAndPort) {
    MONGO_UNREACHABLE;
}

void PinnedConnectionTaskExecutor::appendNetworkInterfaceStats(BSONObjBuilder& bob) const {
    MONGO_UNREACHABLE;
}

}  // namespace mongo::executor
