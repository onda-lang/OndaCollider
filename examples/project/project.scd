(
s.waitForBoot {
    var here = PathName(thisProcess.nowExecutingPath).pathOnly;
    var project = here +/+ "project.ondaproject";

    OndaDef(\project, project).send(s, {
        { Onda.ar(\project, (freq: MouseX.kr(110, 880, 1))) * 0.2 }.play;
    });
};
)
