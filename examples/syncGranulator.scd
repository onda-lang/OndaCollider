(
s.waitForBoot({
    var here = PathName(thisProcess.nowExecutingPath).pathOnly;
    var src = here +/+ "syncGranulator.onda";
    var b;

    b = Buffer.read(s, ExampleFiles.child);
    s.sync;

    OndaDef(\syncGranulator, src).send(s, {
        x = {
            var pos = MouseX.kr(0, 1);
            var rate = MouseY.kr(0.5, 5.0);
            var grainSize = LFNoise1.kr(1).range(0.01, 0.3);

            var sig = Onda.ar(\syncGranulator, (
                buf: b.bufnum,
                pos: pos,
                rate: rate,
                grainSize: grainSize
            ));

            sig ! 2
        }.play;
    });
});
)
