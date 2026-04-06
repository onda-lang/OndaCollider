(
s.waitForBoot({
    var here = PathName(thisProcess.nowExecutingPath).pathOnly;
    var src = here +/+ "syncGranulatorDual.onda";
    var b;

    b = Buffer.read(s, ExampleFiles.child);
    s.sync;

    OndaDef(\syncGranulatorDual, src).send(s, {
        x = {
            var pos = MouseX.kr(0, 1);
            var rate = MouseY.kr(0.5, 5.0);
            var spread = LFNoise1.kr(0.25).range(0.0, 0.12);
            var grainSize = LFNoise1.kr(1).range(0.01, 0.3);

            var sig = Onda.ar(\syncGranulatorDual, (
                buffer: b.bufnum,
                pos: pos,
                rate: rate,
                spread: spread,
                grainSize: grainSize
            ));

            sig ! 2
        }.play;
    });
});
)
