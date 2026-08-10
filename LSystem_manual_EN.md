# LSystem
LSystem is a sequencer of `scale degree/duration` pairs that uses rules which can self-trigger or trigger other rules based on the last scale degree evaluated at the end of each rule. The musical scale can be chosen from the context menu. It can behave deterministically, but things get interesting once you introduce random note or duration events.

These random events can be constrained through 2 text inputs at the bottom of the module that act as a pool of degrees and durations. Or they can be left unconstrained (using the module logic's default limits) if their corresponding pool input is empty.

The module is monophonic and polyphonic up to 6 independent voices, each running its own randomization logic for degrees, durations, and possible paths between rules.

The module needs a clock signal at its Clock input to work. It is designed to receive 48 PPQN (48 pulses = 1 quarter note).

Up to 7 rules can be added in their respective text fields.

If a currently active rule is modified or rewritten, the changes won't take effect until it finishes playing, including all of its repetitions. This lets you edit rules without stopping playback. A rule entry is confirmed with Enter or when the text field loses focus. Focus is shown as a yellow highlight on the horizontal title bar of each rule. This bar turns red if the entered or modified rule contains a syntax error. It turns green whenever the module's first channel plays that rule.

The simplest format for specifying a rule is:
Initiator (degree,duration), the pair of characters `->`, and the sequence in the form of degree,duration [space] degree,duration [space] degree,duration...

## An example using only the first rule field:

`1,1->1,1 3,1/2 5,3 7,4`

Whatever comes before `->` is not sequenced, it's used to label whether this rule can be picked by another one. What comes after is the sequence. In this case: the first scale degree with a duration of 1 beat, the third with a duration of 1/2 beat, the fifth with a duration of 3 beats, and the seventh with a duration of 4 beats. Since the last degree was 7, if no rule is found with that degree as its initiator, this rule will repeat indefinitely.

Let's add a second rule to the example, in the second text field (or any other), which together with the first would look like:

`(Rule 1:) 1,1->1,1 3,1/2 5,3 7,4`
`(Rule 2:) 7,1->5,1 1,1`

Now, since the final degree of the first rule matches the initiator degree of Rule 2, the sequence will continue into Rule 2 once the duration of the last degree (4, in this case) ends.

Notice in the example that the duration of the last degree of Rule 1 and the duration of the initiator of Rule 2 are not the same, but the degree number takes priority, and if two target rules share the same initiator degree, they're disambiguated by duration. If two or more target rules have the exact same initiator (same degree AND same duration), a round robin is applied in row order, top to bottom.

A rule whose last degree number isn't defined as the initiator of any other rule will jump to the first rule or to a random one (available as an option in the context menu).

## Using `r` (random) and `k` (last random value):
The character `r` can be used to specify a random value at any step of a rule: in the degree, the duration, or both at once. Every time `r` is evaluated, it takes a random value between predefined limits in the module's engine, if the corresponding pool is empty. Otherwise, it randomly picks one of the values specified in the pool.

A character that complements `r` is `k`. When `k` is used in the degree, duration, or both, it takes the last value that `r` produced in that respective field — in the same rule or any previously executed rule — or defaults to 1 if `r` was never evaluated. Here:

`1,1->r,r k,k`

`k` will repeat, in the second step, both the degree and the duration that were randomly selected in the first step.

## Adding and subtracting degrees (`+` and `-`) and octaves:
An integer can be added to or subtracted from a degree (or from a list of degrees, explained further below) to raise or lower it. If the result of that operation goes past one end of the limits (set from the context menu), the value wraps around to the opposite end (like a cyclic counter), continuing from there instead of stopping at the limit.

An interesting use of these operators is combining them with `k`, to get a degree that's different from, but relative to, the last random degree evaluated. Example:

`1,1->r,1 k+5,1 -3,1`

It's worth clarifying here that the `-` sign in -3 (third step) doesn't refer to a subtraction performed on each repetition, but to picking the degree one octave lower on the scale. Higher octaves are reached with degrees higher than the scale's last degree.

## Repeating rules with the `*` operator:
Before evaluating its next target rule, a rule can repeat itself using the `*` character together with a number n of repetitions (no space between them, like this: `*n`), specified at the end of the rule. Example:

`1,1->1,1 5,1 *4`

This rule will repeat 4 times before evaluating the possible target of its last degree.

**Only one `*n` operator is allowed per rule.** Using more than one in the same rule (e.g. `1,1->1,1 *2 2,1 *4`) is rejected as a syntax error, rather than silently letting the last one override the earlier one.

When using more than one repetition in a rule, the results of the `+` and `-` operations are preserved across repeats. Example:

`1,1->1,1 1+1,1 *4`

This rule will sequence, with a duration of 1 beat each, the degrees 1 > 2 > 1 > 3 > 1 > 4 > 1 > 5 before repeating again or jumping to another rule.

## Weighted random list:
A list of degree (or duration) numbers enclosed between `<` and `>` can be specified at any step of a rule. Example:

`3,1->1,1 <3,5,7,-3>,1`

This will pick, with equal probability, one of those 4 degrees on every execution of the rule. In this case the duration for the chosen degree is always 1, but you could also write:

`3,1->1,1 <3,5,7,-3>,<1,2>`

Here every degree or duration number has equal probability of being chosen, i.e. a weight of 1. If you want one or more degrees to have a higher chance of being picked, you can use the `:` character followed by a weight number next to the values you want to adjust. Higher weight = higher probability:

`3,1-><r:0.25,5,7,k:2>,<1,2:0.5>`

Note that both `r` (random degree or duration) and `k` (last `r` evaluated for degree or duration) can be used inside these lists.

## Random pool for r (Degrees):
In this input (located below the seventh and last rule field) you can enter a comma-separated list of numbers. Every time `r` is evaluated in the degree of a step, it will randomly pick a value from this list. This list is completely independent from the `<>` lists that can be entered inside individual steps.
Weights can also be assigned to one or more values here: `1:9,2:7,4:1`

## Random pool for r (Durations):
This input is similar and located next to Random pool for r (Degrees). These are the values `r` can take when specified in the duration of a step.

## Using the `^` operator (Glide):
As you may have noticed, every step of a rule is separated from the next by a space. That space can be replaced with the `^` character whenever you want. This means the step to the left of `^` will glide — a linear pitch slide from its own degree toward the pitch of the step to its right. The glide's target step is left unaffected by it, unless that target is itself the start of another glide. The glide's duration equals the duration of the step that starts it. The glide has no effect if either the origin step or the target step is a silence (`s`).

The `^` must sit directly against the step it modifies, with no space in between, in order to work. A space is fine on the other side, between `^` and the target step.

## Silences with the `s` character. Gate and V/Oct outputs:
Every step of a rule sends the voltage corresponding to its degree to the module's V/Oct output, along with a gate that stays high until its specified duration ends. At the exact instant the next step begins, the gate is briefly deactivated for a very short time, to retrigger envelopes or any module connected to the Gate output that needs it.

A step can be silenced by using the `s` character in place of the degree. This deactivates the Gate output for the duration of that step: `1,1->1,1 s,2 3,1`.

## Eor (End of Rule) and Rule (Rule number) outputs:
The Eor output fires a single trigger only when the whole rule finishes playing — including all of its repetitions if it uses the `*n` operator, which count as a single block, not one pulse per repetition.

The Rule output sends a stepped voltage from 0 to 10V according to which rule is currently active, in steps of 10/6 ≈ 1.667V, with 0V being the first rule and 10V the seventh and last.

Good moment to remember that every output of the module is independent per channel in polyphonic mode.

## Run and Reset buttons, Run Toggle and Reset inputs:
The Run button stops or starts the module's sequence, shown by a light on that button.
The Run Toggle input flips the Run button's state every time it receives a trigger.
The Reset button, and its corresponding input, restart the sequence of every channel back to the first rule.
You can specify from the context menu whether a reset is applied every time the sequence is started with the Run button or the Run Toggle input.

## Durations: allowed values and limits.
Time unit: the module works internally in ticks, where 48 ticks = "1" (one beat/quarter note, since it's designed for 48 PPQN at the clock). Everything you write as a duration is converted to ticks on that basis.
Two accepted formats, in any step, initiator, `<>` list, or pool:
Plain number (integer or decimal): represents a number of beats. 1 = 48 ticks, 2 = 96 ticks, 0.5 = 24 ticks, 0.25 = 12 ticks.
Fraction N/D (both unsigned integers): 1/2 = 24 ticks, 3/4 = 36 ticks, 1/8 = 6 ticks.
Minimum limit: any value that would round to less than 1 tick is automatically rounded up to 1 tick (the smallest possible duration, 1/48 of a beat).
Maximum limit: 1,000,000 ticks (≈ 20,833 beats).
These same format rules and limits apply exactly the same way to the values entered in the duration pool.

## Context Menu Options:

**Polyphony channels**: Allows selecting from 1 channel (default) up to 6 independent channels for the V/Oct, Gate, Rule, and Eor outputs.
**Reset when starting playback**: Resets the module whenever playback is started by the Run Toggle input or the Run button. Enabled by default.

**If a note has no rule...** Selects between jumping to the first rule (default) or jumping to a random one when the last degree of a rule has no targets in the initiators.

**Autoreset after steps**: Options to immediately reset the module after a certain number of steps based on the Clock input. Selectable options: Off (default), 8, 16, 32, or 64 steps.

**Scale**: Allows selecting the scale to which the rule degrees will be quantized. Natural major scale by default.

**Root note**: Allows selecting the root note or tonic of the scale. C by default.

**Random degree range(r)**: Allows selecting from various predefined limits within which `r` will pick its value if the [Random pool for r(Degrees)] input is empty. Also applies limits to the `'+'` and `-` operators. `-8 to 16` by default.

## Limitations, tips, and notes:

The accepted rule format can be fairly cryptic, hard to read, or feel limited in space and rule count. Consider this an experimental module with plenty of room for improvement — even so, we think you can get some very interesting results out of it if the rules are programmed well.

You can use more than one space if you like, to separate a rule's steps from each other — but the glide operator needs to sit immediately after the step it modifies in order to work.

Two or more channels of a polyphonic sequence will follow the same path unless they diverge from each other through the different random degree and duration evaluations. Sometimes it's not desirable for a polyphonic oscillator to receive the exact same thing on every channel — for that reason it can be worth splitting the channels at the output stage, to send them to different destinations or spread them across the stereo field.

The RULE output produces a voltage from 0 to 10 depending on which rule (1 through 7) is currently playing for each channel. This can be useful for driving the step-selection input of some other sequencer (like Bogaudio's ADDR-SEQ), scaling it and using it as V/Oct, comparing it against a value to trigger different events, and so on.

Don't forget the system's cut/copy/paste and selection functions, available in each rule's context menu. It can be handy to have a module like VCV's NOTES nearby while you experiment with different rules. You could also use a module like Stoermelder's 8FACE to set up and sequence different pages of rules.

There is currently no option to change the gate's duration, either as a fixed value or relative to the step's duration — but other modules connected to the Gate output could be used to reshape the gate's timing or turn it into a trigger.

## Quick reference of example rules:

**`1,1->1,2 -2,1/2 <1,3>,1 <1,3>,<1,2> r,r k+1,k`**

`1,1` = Initiator: (Degree 1. Duration 1).

`->` = Separator (Initiator -> Sequence).

`1,2` = Step (Degree 1. Duration 2).

`' '` = A space separates steps.

`-2,1/2` = Step (Degree -2. Duration 1/2, as a fraction).

`<1,3>,1` = Step (List format: Degree 1 or 3. Duration 1).

`<1,3>,<1,2>` = Step (Degree 1 or 3. Duration 1 or 2).

`r,r` = Step with random degree and duration, drawn from the pool (if not empty) or unconstrained.

`k+1,k` = Step (Last r degree, increased by 1. Last r duration. (+ and - are not allowed for durations)).


**`2,1-><1,2:5,3:2>,1 2+2,1 5,2^0,1 s,2 *4`**


`<1,2:5,3:2>,1` = Step (Weighted list: Degree 1, 2, or 3. Duration 1). Higher weight = higher probability.

`2+2,1` = Step (Degree 2 increases by 2 on each repetition. Duration 1).

`5,2` = Step (Degree 5. Duration 2).

`^` = Replaces the space separator and applies a glide from the degree of the step on its left toward the degree of the step on its right, over the duration of the step on its left.

`0,1` = Step (Degree 0. Duration 1). Not affected by the preceding glide.

`s,2` = Step (Silence. Duration 2).

`*4` = Number of repetitions for the rule (4).
