﻿# BSR
                           
## Synopsis
block sync & replication (bsr) is an open source project that implements software-based block replication over the network. It is based on a kernel-level replication engine and supports Windows and Linux cross-platform environments. bsr originally started out as a WDRBD project and only supported the Windows environment, but now extends to the Linux environment to support cross-platforms.

## Motivation
We have ported the source code of Linux DRBD 9.0 to Windows via WDRBD and have done it successfully. At present, WDRBD has already been widely deployed and used in enterprise HA environments for years, proving its performance and reliability. But in the process, we had to endure too much pain. The original DRBD9 suffered from all sorts of bugs, including unclear synchronization logic, OOS remaining problems, and synchronization interruptions due to instability of 1:N replication and hasn't solved these problems for years. Based on the DRBD9 engine, we had to analyze the DRBD engine itself, fix it to our taste, and fix various bugs ourselves.

It was not easy to solve these problems, but we finally achieved stabilization of WDRBD, and now we want to extend it to the Linux environment through the bsr project.

bsr will become a block replication cross-platform open source project supporting Windows and Linux based on the bsr engine.

## Improvements
bsr is basically a fork project of DRBD, but it is a more reliable and powerful replication solution that improves a features and solves many of DRBD's problems. There are many improvements, but I would like to introduce the following improvements.

### Stable Sync
bsr has redesigned the 1:N synchronization logic of DRBD9's. Basically, Stable Sync is the main synchronization logic of bsr, By limiting the directionality of synchronization in a 1:N mesh topology, you can clarify which nodes can be SyncSource. In addition, when the role is changed or swtich-over, the trigger is activated by protocol to change the synchronization direction actively. so it is more stable by implementing consistent synchronization logic compared to DRBD9 which performs synchronization without proper criteria.

### Fast Sync
bsr is oriented towards FastSync. FastSync now performs synchronization only fast on disk space that is used on a filesystem basis, making synchronization much faster. It has been implemented in Windows environment and has secured its stability, and we port it for Linux environment. Currently, FastSync supports NTFS, ReFS, ext and xfs file systems.

### Fast OV
Online Verification also operates in Fast mode. Consistency check can be performed quickly by comparing only the file system usage area.

### Kernel Tx Buffering
In the meantime, DRX integration with proxy buffering was required for long-distance replication on Linux. It is now possible to perform kernel-level TX buffering on Linux. We have further increased the performance of asynchronous replication.

### Improved engine logics
We improved performance by fundamentally solving the serious performance degradation problem that DRBD has.

- DRBD9 has a bottleneck in DRBD internal logic and causes local I/O performance degradation when DRBD request increases dramatically in low band replication environment. We have redesigned the logic to manage the replication request separately so that local I/O is not affected even at low bandwidths.
- AL and Resync LRUs are designed to cause problems if both replication and synchronization occur simultaneously in a cross-reference relationship. In real-world replication applications, We have experienced many problems with local bottlenecks caused by AL bottlenecks and delays in synchronization because Resync LRU and AL may interfere with or compete with each other. and so, we minimized the bottlenecks of AL that could affect local I/O and have modified the existing logic to compensate for the behavior of the Resync LRU and AL to not interfere with each other.

### Passthrough Policy
Improved disk I/O error handling logic to introduce a new policy called passthrough. The existing detach policy led to the implementation of a feature called diskless mode, which was unreasonable in HA operation. diskless mode is a good idea but ambiguous logic is required to run with HA, and I/O redirection suffers from performance degradation in DR replication environments. Since disk I/O errors can happen all the time, detaching a disk that is mirroring is an extreme approach. It makes more sense for the file system to provide an opportunity to overcome this by retrying I/O. We devised a passthrough policy that complements the existing logic so that we have the opportunity to overcome I/O errors on our own without having to detach the disk being mirrored.

### Timeout
drbd is implemented with strict processing logic that does not handle timeouts, causing hang issues in many cases. We have validated this problem with a number of operational cases and have stabilized it with exception code through proper timeouts.

## Plan
In the future, we intend to further develop bsr to remain a model for open source projects of block replication solutions. We look forward to your participation and support.

## Contributors
Man Technology Inc.(http://www.mantech.co.kr/), bsr@mantech.co.kr

## License
This project is licensed under the GPL v2
