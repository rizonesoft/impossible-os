# Future Research

Research spikes: studies that decide whether and how to build something large before any code is committed. Each roadmap ends in a plan document and, at most, a throwaway prototype. None of the six below has started, but some of what they asked for has since shipped elsewhere, and each page says exactly what exists today.

## Research spikes

One page per roadmap file, each following the [page contract](../contributing/docs-page-contract.md).

| Document | Topics |
| --- | --- |
| [ARM64 and RISC-V Architecture Port](multi-arch-port.md) | x86-64 construct audit, AArch64 spike, RISC-V delta, calling-convention study |
| [Type-1 Hypervisor (ImpossibleHV)](hypervisor.md) | VT-x and AMD-V, a minimal VMM, a Linux guest, host-side virtio, GPU passthrough |
| [GPU-Accelerated Compositor](gpu-compositor.md) | VirtIO-GPU against native drivers, Mesa softpipe and lavapipe, GPU memory and fences |
| [Secure Boot, TPM 2.0 and Measured Boot](secure-boot-tpm.md) | What shipped under the boot roadmaps; key enrollment, disk encryption and vTPM still open |
| [AI and ML Native Inference Runtime](ai-ml-runtime.md) | Runtime choice, GGUF loading, an inference system call; per-task XSAVE already ships |
| [Android App Compatibility](android-app-compatibility.md) | Android as a VM guest, the options rejected, display and input bridging |

The implementation roadmaps for porting the kernel are in [Architecture Ports](../ports/index.md).
