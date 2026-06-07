# core/engine — Architecture

## Overview

`core/engine` implements a **reactor-based processing engine**: a thread pool that dispatches work to registered *processing objects* (ProcObjs). Each ProcObj owns a private MPSC queue and is guaranteed to run on only one thread at a time, so its internal state never needs external locking.

## Key concepts

### Message

`Message` is the base class for all data exchanged between ProcObjs. Derive from it to carry typed payloads:

```cpp
struct MyMsg : public core::engine::Message {
    int value;
};
```

### ProcObj

`ProcObj` is the unit of work. Subclass it and implement `process_msg()`:

```cpp
class MyObj : public core::engine::ProcObj {
    void configure() override { /* allocate resources */ }
    void start()     override { /* activate */ }
    void stop()      override { /* release */ }

    void process_msg(std::unique_ptr<core::engine::Message> msg) override {
        auto* m = dynamic_cast<MyMsg*>(msg.get());
        // handle m
    }
};
```

Post a message to any ProcObj from any thread:
```cpp
obj->post(std::make_unique<MyMsg>(42));
```

### CoreEngine

`CoreEngine` owns the thread pool and dispatches ready ProcObjs to worker threads:

```cpp
core::engine::CoreEngine engine;
engine.start(4);                              // 4 worker threads

auto obj = std::make_shared<MyObj>();
engine.add_proc_obj(obj);                     // configure() + start() called here

obj->post(std::make_unique<MyMsg>(1));        // queued, dispatched to a free worker

engine.stop();                                // drains workers, calls stop() on all objs
```

## Threading model

```
                    post(msg)
  Any thread ──────────────────► ProcObj queue (MPSC)
                                      │
                              ┌───────▼────────┐
                              │  CoreEngine     │
                              │  ready queue    │
                              └───────┬────────┘
                    ┌─────────────────┼─────────────────┐
                 Worker 0         Worker 1           Worker N
                    │                │                   │
              runs ProcObj A   runs ProcObj B      runs ProcObj C
```

**Rules:**
- A ProcObj is on the ready queue **only when it has pending messages**.
- A ProcObj is processed by **at most one worker at a time** — no concurrent access to its state.
- After draining a ProcObj's queue, the worker checks for messages that arrived during processing and re-schedules if needed, preventing the ABA race.
- Workers block on a condition variable when idle; `stop()` wakes them all.

## Lifecycle

```
add_proc_obj(obj)
    │
    ├─► configure()   ← set up resources, read config
    ├─► start()       ← activate, begin accepting messages
    │
    │   [engine running — messages dispatched to process_msg()]
    │
stop()
    └─► stop()        ← called on all registered ProcObjs
```

## Directory structure

```
core/engine/
├── inc/                   # Public API — include as "core/engine/<name>.hpp"
│   ├── message.hpp        # Message base class
│   ├── proc_obj.hpp       # ProcObj interface
│   └── core_engine.hpp    # CoreEngine
├── src/
│   ├── proc_obj.cpp       # Queue + scheduling logic
│   ├── core_engine.cpp    # Worker thread pool + reactor dispatch
│   └── tst/               # Unit tests (white-box, implementation details)
└── tst/                   # API tests (black-box, public interface only)
    └── test_core_engine.cpp
```

## Running the tests

```bash
bazel test //core/engine/tst:core_engine_test
```
