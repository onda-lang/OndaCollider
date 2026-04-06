(
s.waitForBoot({
    var here = PathName(thisProcess.nowExecutingPath).pathOnly;
    var src = here +/+ "lorenz.onda";

    OndaDef(\lorenz, src).send(s, {
        x = {
            var rho = MouseX.kr(20, 40);
            var speed = MouseY.kr(1, 100);
            Onda.ar(\lorenz, (rho: rho, speed: speed)) * 0.7
        }.play;
    });
});
)
