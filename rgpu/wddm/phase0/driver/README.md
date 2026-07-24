# Driver package boundary

`RemoteGpuRoot.inf` reserves the root-enumerated hardware ID `Root\RemoteGpuRender` and the render-only adapter package boundary. Placeholder binaries are intentionally absent.

Do not install this INF until a real WDDM KMD and UMD exist, the package is catalogued/signed, and testing is performed in a disposable VM. The normal Phase-0 build only validates the INF text and records WDK availability.
