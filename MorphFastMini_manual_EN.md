# MorphfastMini — Manual

**Plugin:** Quostate · **Module:** MorphfastMini

MorphfastMini generates a voltage transition from an **Init** voltage to a **Target** voltage, shaped by a selectable **curve**, timed by an external **clock**, and fired manually or by a trigger. The duration of the transition is specified in whole clock pulses, making it easy to sync morphs to tempo.

---

## Signal Flow

```
Clock ──┐                          ┌──> Out   (transition voltage)
Fire ───┼──> [Delay] -> [Curve] ───┤
Reset ──┘                          └──> EOT   (end-of-transition trigger)
```

A transition always begins on a **clock rising edge**. Once triggered, it progresses over the number of clock pulses set by *Duration*, following the selected *curve* and *easing*, and raises the **EOT** output when done.

---

## Controls

### Knobs

| Knob | Range | Description |
|---|---|---|
| **Init** | −10 V … +10 V | Starting voltage of the transition. |
| **Target** | −10 V … +10 V | Ending voltage of the transition. |
| **Duration** | 01 … 64 | Length of the transition in clock pulses (integer). Default: 08. |

### Buttons

| Button | Behavior |
|---|---|
| **Fire** | Momentary. Starts the sequence that leads to a transition (same as the *Fire* input). |
| **Swap Init/Target** | Momentary. Exchanges the values of the Init and Target knobs. |
| **Curve** | Cyclic. Selects the transition curve: Ramp → Ramp Back → Step → Triangle. |
| **Delay** | Cyclic. Sets a start delay in clock pulses: OFF, 01, 02, 04, 08, 16, 32. |

### Displays

- **Duration display** — Two-digit readout (`01`–`64`) mirroring the Duration knob.
- **DLY display** — Shows the current delay setting (`OFF`, `01`, `02`, `04`, `08`, `16`, `32`).
- **Curve display** — Icon of the selected curve.

### Light

- **Active LED** (yellow) — Lit from the moment a fire is accepted until the transition completes (covers both the delay phase and the transition itself).

---

## Inputs and Outputs

| Jack | Type | Description |
|---|---|---|
| **Clock** | Input | Rising edges time the transition. Also defines its length in real time (one pulse = one step). |
| **Reset** | Input | A rising edge immediately cancels any armed, delayed, or running transition and returns the output to the Init voltage. |
| **Fire** | Input | Trigger that starts a transition. Ignored while a delay or transition is already active. |
| **EOT** | Output | 10 V trigger lasting 10 ms, emitted only when a transition finishes. |
| **Out** | Output | The transition voltage. |

---

## Curves

The **Curve** button cycles through four shapes. The display above the button shows the current selection.

### Ramp
A straight linear glide from Init to Target. When finished, the output **holds Target** until the next transition.

### Ramp Back
A straight linear glide from Init to Target. When finished, the output **returns to Init** and rests there until the next transition.

### Step
A staircase: the output advances one step per clock pulse, taking Init-to-Target in *Duration* pulses. When finished, the output **holds Target**. Easing shapes the height of each step (see below).

### Triangle
Reaches Target at **exactly half** of the transition duration, then retraces back to Init during the second half — a triangular wave over the full transition. When finished, the output **rests at Init** until the next transition (like Ramp Back).

Example with Duration = 08 and a 0 V → 10 V morph: pulses 1–4 climb from 0 V to +10 V; pulses 5–8 descend back to 0 V.

---

## Ease In / Ease Out (context menu)

Right-click the module to access two sliders, expressed in percent:

- **Ease In** softens the *start* of motion.
- **Ease Out** softens the *arrival* at the end of motion.

At 0 % / 0 % all curves are pure linear/staircase shapes.

How they affect each curve:

| Curve | Effect |
|---|---|
| Ramp / Ramp Back | Bends the slope: high Ease In leaves Init slowly and accelerates; high Ease Out decelerates into Target. |
| Step | Reshapes the staircase heights along the same bent profile. |
| Triangle | Applied symmetrically to both halves. **Ease In rounds the departures** (leaving Init, and leaving Target on the way back); **Ease Out rounds the arrivals** (arriving at Target, and arriving back at Init). |

Notes:

- The two controls act as opposing forces on a single profile: only their *difference* matters. Setting both to 100 % cancels them out and yields the plain linear shape again — on every curve. To round a triangle's peak, use Ease Out alone; to round its valleys, use Ease In alone.
- Extreme settings (100 % of either) flatten the corresponding corner completely without ever overshooting the Init/Target voltages.

---

## Timing and Clocking

- A fired transition **always waits for the next clock rising edge** before starting (unless Same-step chaining applies, see below).
- The transition spans exactly *Duration* clock pulses. The module measures the time between clock edges, so the transition's speed automatically follows the clock's tempo.
- Progress only advances while clock cycles are arriving: each pulse moves the transition forward by one step. **If the clock stops mid-transition, the output freezes** where it is and resumes on the next pulse.
- If *Duration* + *Delay* exceed 64 pulses, Duration is clipped so the total never surpasses 64.

### Delay

After a fire is accepted, the transition can be postponed by 0–32 additional clock pulses (Delay button). The delay is counted in clock edges, starting from the edge that arms the sequence. The Active LED stays lit throughout.

### Same-step chaining (context menu option)

Enabled by default. A fire trigger arriving **shortly after a clock edge** (within a small tolerance window — half a millisecond or a quarter of the last clock period, whichever is shorter) is treated as coincident with that edge. This tolerates small cable/chain latencies: instead of waiting a full extra clock cycle, the recent edge acts as the arming edge and the delay (if any) counts from it. Disable this option if you prefer fires to strictly wait for the next clock edge.

---

## Parameter Changes During a Transition

Any change made while a transition (or its delay) is running — knobs, curve, delay, easing — is **ignored until that transition ends**. All relevant values are captured at the instant the transition starts, guaranteeing a clean, glitch-free morph. The new values apply from the next transition onward.

---

## State Persistence

The module saves its **curve selection**, **delay setting**, and **Same-step chaining** option with the patch.

---

## Quick Reference

| Item | Value |
|---|---|
| Init / Target range | ±10 V |
| Duration | 1–64 clock pulses (default 8) |
| Delay | 0, 1, 2, 4, 8, 16, 32 pulses |
| Curves | Ramp, Ramp Back, Step, Triangle |
| EOT trigger | 10 V for 10 ms |
| Reset | Cancels everything, output snaps to Init |
| Context menu | Ease In (%), Ease Out (%), Same-step chain |
