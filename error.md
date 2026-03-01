mog@MOGs-MacBook-Pro mac % swift build -c release
Building for production...
/Users/mog/Documents/GitHub/display-bridge/mac/Sources/DisplayBridgeReceiver/InputForwarder.swift:214:13: warning: variable 'pkt' was never mutated; consider changing to 'let' constant
212 | let inputData = inputEvt.serialize()
213 |
214 | var pkt = DBPacketHeader(
| `- warning: variable 'pkt' was never mutated; consider changing to 'let' constant
215 | type: DB_PKT_INPUT_EVENT,
216 | flags: 0,

/Users/mog/Documents/GitHub/display-bridge/mac/Sources/DisplayBridgeReceiver/MetalRenderer.swift:39:5: error: overriding declaration requires an 'override' keyword
37 | // MARK: - Init
38 |
39 | init?() {
| `- error: overriding declaration requires an 'override' keyword
40 | guard let device = MTLCreateSystemDefaultDevice() else {
41 | print("[renderer] ERROR: no Metal device")

/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX26.2.sdk/usr/include/objc/NSObject.h:66:1: note: overridden declaration is here
64 |
65 | + (void)initialize;
66 | - (instancetype)init
| `- note: overridden declaration is here
67 | #if NS_ENFORCE_NSOBJECT_DESIGNATED_INITIALIZER
68 | NS_DESIGNATED_INITIALIZER

/Users/mog/Documents/GitHub/display-bridge/mac/Sources/DisplayBridgeReceiver/MetalRenderer.swift:39:5: error: failable initializer 'init()' cannot override a non-failable initializer
37 | // MARK: - Init
38 |
39 | init?() {
| `- error: failable initializer 'init()' cannot override a non-failable initializer
40 | guard let device = MTLCreateSystemDefaultDevice() else {
41 | print("[renderer] ERROR: no Metal device")

/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX26.2.sdk/usr/include/objc/NSObject.h:66:1: note: non-failable initializer 'init()' overridden here
64 |
65 | + (void)initialize;
66 | - (instancetype)init
| `- note: non-failable initializer 'init()' overridden here
67 | #if NS_ENFORCE_NSOBJECT_DESIGNATED_INITIALIZER
68 | NS_DESIGNATED_INITIALIZER

/Users/mog/Documents/GitHub/display-bridge/mac/Sources/DisplayBridgeReceiver/ViewController.swift:171:27: error: cannot find type 'CMTime' in scope
169 | func hevcDecoder(\_ decoder: HEVCDecoder,
170 | didOutput pixelBuffer: CVPixelBuffer,
171 | pts: CMTime) {
| `- error: cannot find type 'CMTime' in scope
172 | // Already on main thread (dispatched by HEVCDecoder)
173 | framesDecoded += 1

mog@MOGs-MacBook-Pro mac % pwd
/Users/mog/Documents/GitHub/display-bridge/mac
mog@MOGs-MacBook-Pro mac %
