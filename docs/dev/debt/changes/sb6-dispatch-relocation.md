# libSceAgcDriver

## add Functional

- [`APS5_DISPATCH_RELOCATION=1`](../../../core/libs/prx/libSceAgcDriver/Execution/src/Driver/Dispatch/DispatchCache.cpp) is unverified on GPU hardware and disabled by default. It relocates only low user-data address words proven to be read bases without value uses. High address words, inline buffer descriptor bases, changed captured words, incomplete or specialization-memory plans, registered images and user-data-linked invocations require the ordinary miss path. Relocated entries validate every captured word and rebuild their dispatch recipe; mixed data refresh and descriptor-base relocation remain unavailable. No performance benefit has been measured.
