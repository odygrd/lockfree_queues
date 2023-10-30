<div align="center">
  <h1>lockfree queues</h1>

  <div>
    <a href="https://github.com/odygrd/lockfree_queues/actions/workflows/linux.yml">
      <img src="https://img.shields.io/github/actions/workflow/status/odygrd/quill/linux.yml?branch=master&label=linux&logo=linux&style=flat-square" alt="linux-ci" />
    </a>
    <a href="https://github.com/odygrd/lockfree_queues/actions/workflows/macos.yml">
      <img src="https://img.shields.io/github/actions/workflow/status/odygrd/quill/macos.yml?branch=master&label=macos&logo=apple&logoColor=white&style=flat-square" alt="macos-ci" />
    </a>
    <a href="https://github.com/odygrd/lockfree_queues/actions/workflows/windows.yml">
      <img src="https://img.shields.io/github/actions/workflow/status/odygrd/quill/windows.yml?branch=master&label=windows&logo=windows&logoColor=blue&style=flat-square" alt="windows-ci" />
    </a>
  </div>

  <div>
    <a href="https://opensource.org/licenses/MIT">
      <img src="https://img.shields.io/badge/license-MIT-blue.svg?style=flat-square" alt="license" />
    </a>
    <a href="https://en.wikipedia.org/wiki/C%2B%2B17">
      <img src="https://img.shields.io/badge/language-C%2B%2B17-red.svg?style=flat-square" alt="language" />
    </a>
  </div>
</div>
<br>

- [Introduction](#introduction)
- [SPBroadcastQueue](#spbroadcastqueue)
- [Performance](#performance)
- [License](#license)

## Introduction

This repository is dedicated to providing a collection of lock-free queues in C++.
In the realm of concurrent programming, there are numerous ways to implement these queues, and several other
repositories exist as well. The goal is to rework various queue types while focusing on making a handful of them
efficient, thoroughly tested, and benchmarked.

## SPBroadcastQueue

The SPBroadcastQueue is a versatile queue that can function as both a Single-Producer, Single-Consumer (SPSC) queue and
a Single-Producer, Multiple-Consumer (SPMC) queue, depending on the template arguments provided.

It can handle both simple and more complex data types, ensuring that the producer takes care of creating and destroying
objects in the queue. The producer synchronizes with consumers and waits for the slowest one when the queue is full.

To use this queue, consumers need to first `subscribe` to it, and then they can start consuming messages.
Importantly, all consumers will see all the messages in the queue.

In addition, special attention has been given to optimizing the queue to avoid performance bottlenecks,
such as false sharing and cache issues. This optimization leads to increased throughput, especially when the number of
consumers grows.

## Performance

Throughput benchmark measures throughput between two threads for a queue of 2 * size_t items.

Latency benchmark measures round trip time between two threads communicating using two queues of 2 * size_t items.

For the most accurate benchmark results, it is recommended to run the benchmarks in your own local environment.

| Queue                        | Throughput (ops/ms) | Latency RTT (ns) |
|------------------------------|:-------------------:|:----------------:|
| SPBroadcastQueue 1 consumer  |       436402        |       270        |
| SPBroadcastQueue 4 consumers |       160785        |        -         |

## License

[MIT License](http://opensource.org/licenses/MIT)