(
x.tryPerform(\free);
w.tryPerform(\close);

s.waitForBoot {
    var source = "
params:
  freq = 440.0 {20.0, 20000.0, log, \"Hz\"}
  amp = 0.1 {0.0, 0.5, curve = -4.0}
  harmonic: i32 = 1 {1, 9, step = 2}

init:
  phase = 0.0

block:
  increment = freq * f32(harmonic) / SR

  sample:
    out1 = sin(TWO_PI * phase) * amp
    phase = wrap(phase + increment, 0.0, 1.0)
";

    a = OndaDef(\specSliderDemo, source);

    a.send(s, { |definition|
        SynthDef(\specSliderDemo, {
            var freq = \freq.kr(
                spec: definition.specs[\freq]
            );
            var amp = \amp.kr(
                spec: definition.specs[\amp]
            );
            var harmonic = \harmonic.kr(
                spec: definition.specs[\harmonic]
            );
            var signal = Onda.ar(definition, (
                freq: freq,
                amp: amp,
                harmonic: harmonic
            ));

            Out.ar(0, signal ! 2);
        }).add;

        s.sync;
        x = Synth(\specSliderDemo);

        {
            w = Window(
                "Onda parameter specs",
                Rect(200, 200, 520, 112)
            ).front;

            EZSlider(
                w,
                Rect(10, 10, 500, 24),
                "Frequency",
                definition.specs[\freq],
                { |slider| x.set(\freq, slider.value) },
                labelWidth: 80,
                unitWidth: 30
            );

            EZSlider(
                w,
                Rect(10, 42, 500, 24),
                "Amplitude",
                definition.specs[\amp],
                { |slider| x.set(\amp, slider.value) },
                labelWidth: 80
            );

            EZSlider(
                w,
                Rect(10, 74, 500, 24),
                "Harmonic",
                definition.specs[\harmonic],
                { |slider| x.set(\harmonic, slider.value) },
                labelWidth: 80
            );

            w.onClose_({
                x.free;
                x = nil;
            });
        }.defer;
    });
};
)
