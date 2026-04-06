(
s.waitForBoot({
    var here = PathName(thisProcess.nowExecutingPath).pathOnly;
    var src = here +/+ "fxChain.onda";
    var b;

    b = Buffer.read(s, ExampleFiles.child);
    s.sync;

    OndaDef(\fxChain, src).send(s, {
        x = {
            var input = PlayBuf.ar(1, b, loop: 1);
            var drive = MouseX.kr(1, 20);
            var feedback = MouseY.kr(0, 0.9);

            Onda.ar(\fxChain, (
                in1: input,
                drive: drive,
                delayTime: 0.2,
                feedback: feedback
            )) ! 2
        }.play;
    });
});
)
