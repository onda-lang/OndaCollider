(
s.waitForBoot({
    var here = PathName(thisProcess.nowExecutingPath).pathOnly;
    var src = here +/+ "dualOsc.onda";

    OndaDef(\dualOsc, src).send(s, {
        x = {
            var freq = MouseX.kr(20, 500, 1);
            var depth = MouseY.kr(1, 5);
            var detune = LFNoise1.kr(0.5).range(0, 12);
            Onda.ar(\dualOsc, (freq: freq, modDepth: depth, detune: detune)) * 0.5
        }.play;
    });
});
)
