(
s.waitForBoot({
    var here = PathName(thisProcess.nowExecutingPath).pathOnly;
    var src = here +/+ "svf.onda";

    OndaDef(\svf, src).send(s, {
        x = {
            var input = WhiteNoise.ar(0.5);
            var cutoff = MouseX.kr(100, 10000, 1);
            var q = SinOsc.kr(0.5).range(0.05, 4.0);
            var mode = MouseY.kr(0, 3);
            Onda.ar(\svf, (
                in1: input,
                cutoff: cutoff,
                q: q,
                mode: mode
            )) ! 2
        }.play;
    });
});
)
