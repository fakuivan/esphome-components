# esphome-components

Assume components are vibe coded unless stated otherwise. Tests are sometimes
provided and I mostly use all of these, but keep in mind that there might be
latent errors. Of course I do review and guide agents when creating these, but
you can't always be too careful with these things.

## Components

### Freshness

Works kind of like a composible watchdog timer. The goal is to detect when a
a series of updates has not occurred for a set deadline. This can be useful
for example on devices that do fixed polling on different variables or states.

I personally use it to detect when a solar inverter is connected to a battery
adapter gateway by bumping the watchdogs from modubs requests.

### Hook and Signal

Pretty generic components that immitate a `global` component but with hookable
state changes. Changing the state is not required for the signal to fire
however.

This is pretty useful for decoupling data providers like a CAN bus data parser
with data consumers like display and API sensors.

### Pre-globals include

Useful for when you need to use custom types on components like `signal` or
`globals`. It's inspired on https://github.com/esphome/esphome/pull/14715.
Since that PR is part of core, it cannot be tested using `external_components`,
thus the existance of this one.

## AI commandments

### Base

1. Do not use default states when those defaults cannot be explained plainly,
   and avoid zero-initializing everything. Leave open the possibility that every
   property may need explicit initialization; that encourages caution on the
   user side and reduces confusion when defaults are not obvious.
2. When you want to transform an `enum class` value into a printable string,
   returning `"unknown"` or something similarly vague from the `default` branch
   of a `switch` is not a good idea. Either return `optional<>` or raise an
   error there to signal that the code is out of sync with the possible enum
   states.
3. Logging an error condition is not error handling. If a programmer assumption
   is broken, we should either `assert` or have a meaningful way to handle the
   issue. Logging errors is sometimes welcome, but it is not something to rely
   on.
4. Bit-manipulation code should use explicitly named functions or helpers
   instead of magic constants and raw binary operations. Using something like
   `BIT(3) & may_have_bit_2` is much better than using `0x4 & may_have_bit_2`.
5. Totally pure functions (such as math helpers, or functions that are not
   constrained by business logic like most other functions in the file) should
   be split into their own file. This is not a strict rule and can be ignored
   when only a couple of functions are like this.
6. Make your types meaningful. Avoid loosely formed types that allow invalid
   states. This prevents you from having to either trust that function inputs
   are valid or re-check those inputs repeatedly.
   
   Sometimes dependent variables need to be hidden behind a class to keep them
   in sync. In those cases, prefer a nested approach: store the interdependent
   state in a class where every property is part of that graph, then nest that
   class into the larger state via `struct`s or similar.
   
   This also applies to common patterns like the builder pattern, or to classes
   with an `init` function. What happens if you call other methods before
   `init`? You then need to check whether `init` has been called and `assert`
   if it has not. In many cases, just using `optional<>` and letting it assert
   on uninitialized access gives you the same functionality without having to
   check in every method call. These patterns cannot always be avoided, but try
   not to use them for your internal types at least.
7. Do not be afraid to use modern constructs like `expected<>`, `optional<>`,
   `array<>`, or other library data structures. If performance becomes an issue,
   they can later be adapted into purpose-built types that achieve a similar
   effect but are tailored to your use case.
8. State your expectations with `assert`. Not doing so can lead to silent errors
   or undefined behavior. `assert` can usually be disabled at compile time, so
   there can be zero performance impact.
9. Prefer small, named functions, but do not make them so small that the
   intermediate states are no longer obvious when calling them. Function scopes
   are very useful for constraining interdependent states that will produce an
   output but must be carefully constructed to avoid invalidity. You can usually
   trust your internal function variables, but you cannot trust that invalid
   states will never be handed to you.
10. When doing errors-as-values, if errors are modeled as enums or variants, do
   not try to reuse those enums throughout multiple functions unless you have a
   very good reason. It is important for the caller to understand the error
   domain of the function being called. If you merge every possible error reason
   under one type and return that, error handling becomes obscure.
   Errors-as-values are supposed to be actionable, so what are we supposed to do
   if we get an error that cannot logically be caused by the function? At that
   point, we are back to invalid values; we have just used a type that can
   represent invalid states.

### esphome specific

* The `init` pattern is pretty common on esphome, stick to it for esphome-facing
  classes, but for internal stuff don't even consider it.
* esphome has some useful stack-only or alloc-once containers that can be used
  instead of the `std` ones.
* Sometimes when data on a buffer is dropped or some condition occurs that is
  not considered a hard fault within a component, ESP_LOGW is used to warn the
  user on the console. It's sometimes nice to also expose a counter or callback
  within the component to have something actionable that does not depend on
  sniffing the logs from within the device.
* You can read the `.ai/instructions.md` file on the esphome repo for more
  context on how sensitive to memory usage this project is. However our
  components do not need to adhere to **small code size** requirements most of
  the time. That limitation is mostly for old devices. However one thing we
  should always strive for is no heap allocations besides initialization, heap
  fragmentation is something that affects every one of these devices, and is
  most of the time easy to avoid without much impact to code versatility and
  correctness.
