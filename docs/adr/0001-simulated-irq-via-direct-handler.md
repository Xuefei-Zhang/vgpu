# Simulated interrupt via direct handler call

The project needs a real top-half/bottom-half interrupt path (a core learning goal), but a genuine IRQ line would require a custom QEMU PCI device — rebuilding QEMU plus devicemodel C code. We chose the mainline vgem approach instead: when the Engine finishes a Submission it calls the IRQ handler directly, as if hardware had rung the line (full flow in DESIGN.md §中断路径).

**Considered options**: (a) custom QEMU PCI device with `pci_irq_raise` — a real line, but a second codebase plus a QEMU rebuild; (b) timer/kthread polling — no top/bottom-half structure to learn; (c) direct handler call (chosen) — vgem precedent, zero extra infrastructure, identical code shape to real hardware.

**Consequences**: the "top half" never executes in a true hardirq context (it runs in workqueue context). Spinlock / no-sleep discipline is still honored in code shape and called out in comments and the README. A real IRQ line is stretch item #1 — it does not change the driver's structure, only the trigger source.
