# Voltronic VM IV Pylontech Query Summary

## Who wrote this

This summary was written by GitHub Copilot, an AI coding assistant using GPT-5.4.

It summarizes protocol analysis and tooling work done during this conversation. It is not a manufacturer document and should be treated as engineering notes based on observed traffic, the Pylontech RS485 protocol transcription, and the test scripts in this folder.

## What I did

During this conversation I:

- inspected the existing ESPHome and complementary Pylontech RS485 emulator code
- reviewed captured inverter traffic in `logger_responder_results.txt` and `logger_responder_results_with_battery_values.txt`
- read the Pylontech RS485 protocol transcription in the separate protocol notes repository
- created and refined `simple_logger_responder/main.py` so it can log traffic and answer a limited set of Pylontech commands for inverter testing
- added `simple_logger_responder/requirements.txt` with `pyserial>=3.5`
- added `simple_logger_responder/analyze_log.py` to summarize captured traffic
- analyzed which commands the inverter sends and how often it sends them
- compared the observed behavior with the protocol document

## Main findings

### 1. The inverter first probes protocol version

The first important query seen was `0x4F` (get protocol version). The returned version changes the inverter behavior.

- If the responder reports protocol version `0x20`, the inverter proceeds with additional battery queries.
- The observed follow-up queries were primarily `0x42` and `0x92`.
- Earlier logs also showed `0x61` appearing on the `0x20` path, so support for that path was added to the responder.

This means `0x4F` alone is not enough for realistic inverter testing. It is only the handshake that selects the next polling behavior.

### 1a. Observed behavior by reported protocol version

The version-probing capture in `logger_responder_results.txt` showed that the inverter does not use the same query sequence for every reported protocol version.

#### Reported version `0x20`

Observed sequence:

- the inverter sent `0x4F` repeatedly at the start
- after receiving `0x20`, it switched to `0x61` on address `0x02`
- when no valid `0x61` reply was available in that early test, it retried `0x61` several times
- it then moved on to `0x42` analog-value queries starting at `0x02` and continuing upward through the pack addresses

Later, once dummy replies were implemented for the `0x20` path, the richer capture showed the stable repeating pattern:

- `0x42` across `0x02` through `0x09`
- `0x92` across `0x02` through `0x09`
- repeated polling loop with roughly 10.2 seconds between polls for the same address/command pair

This was the most complete and most operationally useful behavior observed.

#### Reported version `0x22`

Observed sequence in the available short capture:

- `0x4F` once
- `0x42` analog-value queries across `0x02` through `0x09`

No `0x61` was seen in this short run. No `0x92` was seen before the capture was stopped, so the only safe statement is that `0x42` scanning was definitely observed for `0x22`.

#### Reported version `0x25`

Observed sequence:

- `0x4F` once
- `0x42` analog-value queries across `0x02` through `0x09`
- `0x92` charge/discharge-management query at least on `0x02`
- then the inverter restarted the `0x42` address sweep

This indicates that `0x25` was also accepted as a protocol path that leads to per-pack polling, not just a version probe.

#### Reported version `0x33`

Observed sequence:

- `0x4F` once
- `0x42` analog-value queries across `0x02` through `0x09`
- `0x92` query at least on `0x02`
- then another `0x42` sweep started

The behavior looked very similar to `0x25` in the time window captured.

#### Reported version `0x35`

Observed sequence:

- `0x4F` once
- `0x42` analog-value queries across `0x02` through `0x09`
- `0x92` query at least on `0x02`
- then the inverter returned to the `0x42` sweep and continued polling

This again looked like a per-pack polling mode, similar to `0x25` and `0x33` in the capture length available.

#### Summary of differences

The clearest version-dependent difference observed was:

- `0x20` uniquely showed an initial `0x61` query path in the early capture
- `0x22`, `0x25`, `0x33`, and `0x35` all clearly triggered `0x42` per-pack scanning
- `0x25`, `0x33`, and `0x35` also clearly reached `0x92` in the captured window
- `0x22` may or may not also reach `0x92`, but the capture ended before that was confirmed

So the important practical result is not that only `0x20` works. Several versions caused the inverter to continue into pack polling. What made `0x20` special in this conversation was that it was the version path chosen for dummy-response implementation, and it was the only one where the early capture showed the extra `0x61` behavior.

#### Alternative explanation for the `0x61` observation

An alternative hypothesis remains plausible and should be kept in mind when interpreting the logs.

Hypothesis 1:

- the reported protocol version really does influence the inverter query sequence
- under this interpretation, `0x20` selects a path where the inverter tries `0x61` before settling into the later polling cycle

Hypothesis 2:

- the `0x61` query may not have been caused by the reported protocol version at all
- instead, `0x61` may simply have been part of a generic startup or fallback sequence that happened to appear in the first recorded run
- because `0x61` was not answered in that early test, the inverter may have retried it a few times and then fallen back to `0x42`
- later tests with other versions may have skipped that phase due to internal inverter state, timing, retry logic, or simple chance

At the moment the evidence does not fully separate these two explanations. The strongest safe statement is:

- `0x61` was observed only in the captured `0x20` run so far
- this does not yet prove that `0x20` itself is what gated `0x61`

So any statement that `0x20` uniquely enables `0x61` should be treated as tentative rather than established.

### 2. The observed normal polling pattern was per-pack, not just one global battery query

In the richer capture, the inverter repeatedly queried these addresses:

- `0x02`
- `0x03`
- `0x04`
- `0x05`
- `0x06`
- `0x07`
- `0x08`
- `0x09`

For each of those addresses it requested:

- `0x42` get analog values
- `0x92` get charge/discharge management info

The capture analyzer showed a repeating scan across all of those addresses, roughly every 10.2 seconds per address.

### 3. Those addresses are pack addresses within a battery group

Based on the Pylontech protocol document:

- addresses start at `0x02`
- `0x02` is the first battery position in the group
- `0x03` is the next battery position
- and so on upward

So the inverter was acting as if it expected a multi-pack stack, not a single global battery endpoint.

### 4. Responding on every address implies that multiple packs exist

This is the most important practical conclusion from the conversation.

If a responder answers all addresses from `0x02` through `0x09` with valid per-pack data, the inverter will likely infer that all of those packs are present.

If the same one-pack values are repeated on every address, the inverter may conclude that:

- multiple packs are installed
- total capacity is much larger than reality
- total charge current limit is much larger than reality
- total discharge current limit is much larger than reality

Voltage is less misleading because parallel packs share similar voltage, but capacity and current limits would be overstated if the inverter sums per-pack values.

### 5. If only one real pack exists, cloning it to every address is a functional fake, not an accurate model

If the inverter refuses to operate unless all eight addresses answer, replying on all addresses may still be useful as a compatibility experiment. But that would emulate an eight-pack stack, not a one-pack installation.

For safer emulation, if multiple fake addresses must be answered, the less-wrong approach is to divide single-pack totals across the fake modules for values that are normally summed by the inverter, especially:

- remaining capacity
- total capacity
- max charge current
- max discharge current

### 6. The current responder behavior was intentionally limited

The Python responder was updated to answer only the commands needed for the observed path:

- `0x4F` get protocol version
- `0x42` get analog values
- `0x61` get battery system analog data
- `0x92` get charge/discharge management info
- optional `0x96` get software version when configured

The extra dummy battery-data replies were gated so they only activate when the configured protocol version is `0x20`. If the user selects another protocol version, only the version-handshake behavior is kept.

## Tooling created in this folder

### `simple_logger_responder/main.py`

Purpose:

- listen on RS485
- log raw and parsed frames
- answer a small subset of Pylontech commands with configurable dummy values

Important defaults added during this work:

- charge voltage `54.0 V`
- low cutoff voltage `46.0 V`
- max charge current `10 A`
- charged-state defaults such as high SOC and zero current

### `simple_logger_responder/analyze_log.py`

Purpose:

- parse logs generated by `main.py`
- count observed RX commands
- count emitted TX replies
- show handled vs unhandled requests
- summarize polling intervals by command and address

This was used to confirm that the richer capture contained only handled `0x4F`, `0x42`, and `0x92` queries and that the inverter was polling addresses `0x02` through `0x09` in a repeating cycle.

## Recommended interpretation

Based on the traffic seen in this conversation:

- the inverter appears configured for, or at least expecting, a multi-pack Pylontech-style battery stack
- a single-pack responder that only answers `0x02` is the honest model of one real pack, but it may not satisfy the inverter
- a responder that answers `0x02` through `0x09` can keep the inverter talking, but it represents multiple packs and should not blindly duplicate one-pack current and capacity values across all addresses

## Limitations

These notes are based on:

- captured traffic from one inverter setup
- the Pylontech RS485 protocol transcription available in the related notes repository
- behavior implemented in experimental Python tooling

They are not proof of the inverter's full decision logic. The inverter may still request additional commands later, such as alarm or system parameter queries, depending on configuration and runtime state.