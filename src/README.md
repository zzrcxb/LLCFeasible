# List of Programs

Note that if you are running on a multi-socket machine,
it's recommended to use `numactl` to pin the program to one of the sockets.
This is because we use a helper thread to help construct LLC/SF eviction sets
and we want it to run on the same socket as the main thread.

## Terminologies
+ **Last-level cache (LLC)**, it is the L3 cache on Intel server processors.
+ **Snoop Filter (SF)**, Intel's terminology for cache coherence directory.

## `osc-single-evset`

This program constructs a single L2/LLC/SF eviction set.

The program takes the following arguments:
```bash
osc-single-evset <target cache>
```
The `<target cache>` can be `L2`, `LLC`, or `SF`.

The program also takes the following optional arguments:
+ `-A`, `--algorithm`: name of the eviction set construction algorithm.
    It should be chosen from:
    + `vila`: The baseline group testing algorithm, corresponding to the `Gt` configuration in the paper;
    + `vila-noearly`: The optimized group testing algorithm, corresponding to the `GtOp` configuration in the paper;
    + `vila-random`: Group testing but randomly splits the candidate set, an algorithm discussed by [Song et al. 2019](https://www.usenix.org/conference/raid2019/presentation/song);
    + `ps`: The baseline Prime+Scope implementation, corresponding to the `Ps` configuration in the paper;
    + `ps-opt`: The optimized Prime+Scope implementation, corresponding to the `PsOp` configuration in the paper;
    + `straw` (**default**): Our new eviction set construction algorithm, corresponding to the `Ours` configuration in the paper; and
    + `straw-alt`: Our new eviction set construction algorithm with an alternative backtracking algorithms. It is not used in the paper.
+ `-T`, `--timeout`: Algorithm timeout, measured in milliseconds. Set to `0` to disable timeout (default).
+ `-R`, `--max-tries`: Maximum number of attempts before declaring failure. It is `10` by default.
+ `-B`, `--max-backtrack`: Maximum number of backtracks within an attempt. It is `20` by default.
+ `-C`, `--cands-scale`: Set the candidate set size to: `floor(cands_scale * uncertainty * associativity)`. It is `3` by default.
+ `-f`, `--no-filter`: Disable candidate filtering.
+ `-H`, `--hugepage`: Use huge pages to build eviction sets. This option may fail if huge page is not enabled or unavailable.
+ `-s`, `--single-thread`: When building eviction sets for LLC or SF, we use a helper thread (similar to what Prime+Scope did). This option disables the helper thread. The algorithms generally have worse performance and accuracy in this mode, potentially due to the dead cacheline prediction in Intel server processors. This option is not available to Prime+Scope-based algorithms (i.e., `ps` and `ps-opt`).

### Outputs
Here are some output segments from running
```bash
./osc-single-evset SF -A straw
```

```
INFO: Algorithm: straw
INFO: 22 L3 slices detected, does it look right?
INFO: Cache latencies: L1D: 32; L2: 38; L3: 68; DRAM: 164
INFO: Cache hit thresholds: L1D: 34; L2: 50; L3: 106
INFO: Latency upper bound for interrupts: 820
```
This segment contains automatically detected system information and thresholds.
We use some heuristics to determine the number of L3 slices and
those heuristics can fail on some processors.
Please double check the detected number of L3 slices.
You can override it by setting environment variable `NUM_L3_SLICES=<count>`.

```
INFO: Filtered 23232 lines to 1200 candidates
INFO: L2 Filter Duration: 17491us
INFO: Alloc: 0us; Population: 0us; Build: 1294us; Pruning: 79us; Extension: 0us;
Retries: 0; Backtracks: 0; Tests: 126; Mem Acc.: 35365;
Pos unsure: 0; Neg unsure: 0; OOH: 0; OOC: 0; NoNex: 0; Timeout: 0
Pure acc: 33455; Pure tests: 96; Pure acc 2: 31351; Pure tests 2: 88
INFO: Retry dist:
INFO: Backtrack dist:
INFO: Meet: 0; Retry: 0us
```
Many performance stats from the eviction set construction.
TODO: explanation of each stat.

```
INFO: Duration: 1.296ms; Size: 12; Candidates: 1200
INFO: LLC EV Test Level: 2
INFO: SF EV Test Level: 2
```
This segment shows the duration of constructing the eviction set,
the size of the eviction set, and the candidate set size (after filtering).
Field `LLC/SF EV Test Level` tells you whether the eviction set
can evict the target line from the LLC or SF respectively.
It has the following possible level:
+ `2`: Eviction set can evict the target line (high confidence)
+ `1`: Eviction set can evict the target line (low confidence)
+ `-1`: Eviction set cannot evict the target line (low confidence)
+ `-2`: Eviction set cannot evict the target line (high confidence)

### Debug Mode
If you have `PTEditor` installed and loaded,
and you are running on an Intel server processor,
you can print additional debug information by running the program as `root`.

Here's a sample debug output from running
```bash
sudo ./osc-single-evset SF -A straw
```
(common output omitted).

```
Target: 0x7f5c26ba5140; hash=0x900000705
 0: 0x7f5c244ce140 (hash=0x900000705)
 1: 0x7f5c2438e140 (hash=0x900000705)
 2: 0x7f5c23a5e140 (hash=0x900000705)
 3: 0x7f5c2286e140 (hash=0x900000705)
 4: 0x7f5c2275e140 (hash=0x900000705)
 5: 0x7f5c226ae140 (hash=0x900000705)
 6: 0x7f5c21e3e140 (hash=0x900000705)
 7: 0x7f5c255ee140 (hash=0x900000705)
 8: 0x7f5c21c76140 (hash=0x900000705)
 9: 0x7f5c25f2e140 (hash=0x900000705)
10: 0x7f5c260ce140 (hash=0x900000705)
11: 0x7f5c21016140 (hash=0x900000705)
Match: 12
```
In this debug information,
the first line outputs the virtual address (VA) of the target line
and a hash value that represents to which LLC/SF set the line maps.
The subsequent line shows the VAs and hashes of each address in the eviction set,
and how many of them are mapped to the same LLC/SF set as the target line.

## `osc-multi-evset`

This program constructs multiple eviction sets
for LLC/SF sets at some page offsets or
every LLC/SF set in the system.

The program takes the following arguments:
```bash
osc-multi-evset <number of offsets>
```
If `<number of offsets>` is set to `0`,
the program constructs eviction sets for every LLC/SF set in the system;
otherwise, the program randomly selects `<number of offsets>` offsets
and construct eviction sets for LLC/SF sets at those offsets.
This program takes the same optional arguments as the `osc-single-evset`, except for having no `--hugepage` option and an additional `-L`/`--total-run-time-limit` option
that controls how long the program can run in minutes.

### Outputs
Here's a segmented sample output from running
```bash
./osc-multi-evset 3 -A straw
```
(system information and performance stats omitted).

```
INFO: Offset 0xa40: 704/704/704 (LLC/SF/Expecting)
INFO: Offset 0x940: 704/704/704 (LLC/SF/Expecting)
INFO: Offset 0xc40: 703/703/704 (LLC/SF/Expecting)
INFO: Aggregated: 2111/2111/2112 (LLC/SF/Expecting)
```
This output shows how many LLC and SF eviction sets are successfully constructed
at each page offset.
It shows results aggregated across page offsets in the end.

## `osc-covert`

This program corresponds to the experiment in Section 6.1:
"Fine-Grained Monitoring of Victim Memory Accesses".
At a very high level,
this program establishes a "covert channel" that
the sender thread accesses a target cache line at a certain time interval
while the receiver thread tries to detect those accesses with different
Prime+Probe strategies.
This "covert channel" is based on SF.


This program uses *parallel probing* by default and
takes the following optional arguments:
+ `-p`, `--prime-scope`: use the Prime+Scope-Flush strategy.
+ `-s`, `--use-sense`: together with `-p` enables the Prime+Scope-Alt strategy.
+ `-c`, `--ptr-chase`: probing uses pointer chasing instead of overlapped accesses, conflicting with `--prime-scope`.
+ `-m`, `--monitor-only`: do not spawn a sender thread and just monitor background memory accesses to the SF set that the target line maps to.
+ `-n`, `--num-emits`: number of sender accesses. When using the `monitor-only` mode, this option controls how many accesses we record.
+ `-r`, `--rec-scale`: this sets the maximum number of access records to `rec-scale * num-emits`. Its default value is `20`. This option is ignored in the `monitor-only` mode.
+ `-i`, `--emit-interval`: the period of sender's accesses, measured in cycles. Its default value is `100_000` cycles.
+ `-t`, `--secret-time-scale`: when enabled, the time interval between sender's accesses is randomly chosen between two possible values---`emit-interval` and `floor(emit-interval * secret-time-scale)`---with 50-50 chances. This option simulates a victim with a secret-dependent execution time of an iteration.
+ `-a`, `--secret-access`: when enabled, the sender may randomly skip an access with a 50% chance. This option simulates a victim that makes secret-dependent accesses.

### Outputs
Here are some segments of a sample output by executing
```bash
./osc-covert -n 100
```

```
INFO: Para Probe: no access: 52; has access: 86; threshold: 70; otc: 1; utc: 0
Para Probe: max no acc: 352 68 66 66 66 66 66 66 66 66 66 66 66 66 66 64 64 64 64 64
Para Probe: min acc: 78 78 78 78 78 78 78 78 78 78 78 78 78 78 78 78 78 80 80 80

INFO: Ptr-Chase Probe: no access: 186; has access: 230; threshold: 210; otc: 5; utc: 53
Ptr-Chase Probe: max no acc: 232 222 216 216 216 202 202 202 202 202 200 200 200 200 200 200 200 200 200 200
Ptr-Chase Probe: min acc: 194 198 198 200 200 200 202 202 204 204 204 204 204 204 204 204 204 204 204 204

INFO: Para. Resolution: 96 cycles; Ptr-Chase Resolution: 211 cycles; PS Resolution: 74 cycles
```
The first two segments output calibrated latencies of using parallel probing and pointer-chasing probing.
Each segment first shows the median probing latencies when the monitored set is or is not accessed.
Then, it shows the top-20 probing latencies when the set is not accessed and
the bottom-20 probing latencies when the set is accessed.
The last segment shows the average probing resolutions of different probing methods.

```
Switched: start: 2052381650320120; end: 2052381650342828
Switched: start: 2052381650616036; end: 2052381650639386
Switched: start: 2052381652185028; end: 2052381652219864
...
```
This segment lists time periods that the receiver thread is potentially switched out.
The `start` and `end` fields are the timestamp counter values when the switch starts and ends.

```
Emit  0: tsc: 2052381647877802; aux: 8; lat: 68; bit: 1
Emit  1: tsc: 2052381647977854; aux: 8; lat: 70; bit: 1
Emit  2: tsc: 2052381648077902; aux: 8; lat: 68; bit: 1
...
```
This segment lists the sender's access records. From left to right, the fields are:
(1) timestamp of the access ; (2) ID of the core that the sender runs on;
(3) latency of the sender access, measured in cycles; and
(4) whether the sender makes an access (due to the `-a`/`--secret-access` option).

```
Recv  0: tsc: 2052381647878046; aux: 2; iters: 822; lat: 86; blind: 948
Recv  1: tsc: 2052381647978108; aux: 2; iters: 1662; lat: 88; blind: 1166
Recv  2: tsc: 2052381648078134; aux: 2; iters: 2500; lat: 122; blind: 1068
...
```
This segment lists the receiver's detection records. From left to right,
the fields are:
(1) timestamp of the detection; (2) ID of the core that the receiver runs on;
(3) iteration number of this detection;
(4) probing latency, measured in cycles; and
(5) priming latency, measured in cycles.

```
INFO: Sender emitted: 100; Sender evicted: 100; Rough detection: 99; Slipped: 1
INFO: Spurious count: 3
```

This segment lists (1) how many accesses the sender makes;
(2) how many times the sender's line is evicted;
(3) how many sender accesses are detected by the receiver within an error bound of 1000 cycles;
(4) how many sender accesses are missed by the receiver due to the receiver being switched out; and
(5) how many detected events are spurious events (a spurious event can be caused by a context switch during probing).

## `osc-activity`

This program monitors how often a random LLC set is accessed
due to background activities.
This program is used to make Figure 2 of the paper.

This program takes only two optional arguments:
+ `-n`, `--num-recs`: how many LLC accesses to record.
+ `-r`, `--retry`: the maximum number of retries of constructing an LLC eviction set and calibrating the probing latency.

### Outputs

It outputs results to `stdout`.
Each line of the outputs contains two numbers:
the number of cycles and
the number of monitoring iterations between two LLC accesses.
