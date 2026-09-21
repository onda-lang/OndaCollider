OndaControlSpec : ControlSpec {
	constrain { |value|
		var plain = value.asFloat;
		var clipped = if(plain.isNaN) { minval } { plain.clip(minval, maxval) };

		if(step <= 0.0) { ^clipped };
		^(minval + (((clipped - minval) / step).round * step)).clip(minval, maxval)
	}

	map { |value|
		^this.constrain(warp.map(value.clip(0.0, 1.0)))
	}

	unmap { |value|
		^warp.unmap(this.constrain(value))
	}
}

OndaDef {
	classvar <all, <definitionIds, <generations;
	classvar nextDefinitionId;

	var <key;
	var <id;
	var <generation;
	var <source;
	var sourcePath;

	var <ins;
	var <outs;
	var <specs;

	*initClass {
		all = IdentityDictionary.new;
		definitionIds = IdentityDictionary.new;
		generations = IdentityDictionary.new;
		nextDefinitionId = 0;
	}

	*new { |key, source|
		^super.new.init(key, source);
	}

	*definitionIdFor { |key|
		var symbol = key.asSymbol;
		var definitionId = definitionIds[symbol];

		if(definitionId.isNil) {
			if(nextDefinitionId >= 65536) {
				Error("OndaDef: Definition id limit (65536) reached.").throw;
			};

			nextDefinitionId = nextDefinitionId + 1;
			definitionId = nextDefinitionId;
			definitionIds.put(symbol, definitionId);
			generations.put(symbol, 0);
		};

		^definitionId
	}

	*nextGenerationFor { |key|
		var symbol = key.asSymbol;
		var next = (generations[symbol] ? 0) + 1;

		// Definition ids and generations cross the UGen boundary as exactly
		// representable f32 integers.
		if(next > 16777215) {
			Error("OndaDef: Generation limit reached for '%'.".format(symbol)).throw;
		};

		generations.put(symbol, next);
		^next
	}

	init { |argKey, argSource|
		var src, srcPath, isSourcePath;

		key = argKey.asSymbol;
		id = this.class.definitionIdFor(key);
		specs = IdentityDictionary.new;

		src = argSource.asString;
		srcPath = PathName(src);
		isSourcePath = [\onda, \on, \ondaproject].includes(srcPath.extension.asString.toLower.asSymbol);

		if(isSourcePath) {
			var fullPath = srcPath.fullPath;
			if(File.exists(fullPath)) {
				sourcePath = fullPath;
				source = fullPath;
			} {
				"Invalid path: '%'".format(fullPath).error;
				^this;
			}
		} {
			source = src;
		};
	}

	send { |server, action|
		var compileGeneration;

		server = server ? Server.default;
		if(source.isNil) {
			"OndaDef '%': No valid source to send.".format(key).error;
			^this
		};
		if(server.serverRunning.not) {
			"OndaDef: Server not running. Definition not sent.".warn;
			^this
		};

		compileGeneration = this.class.nextGenerationFor(key);
		generation = compileGeneration;

		forkIfNeeded {
			var cond = Condition(false);
			var compilePath = sourcePath;
			var temporaryPath;
			var compileSucceeded = false;
			var oscFunc;
			var decodeReplyField = { |encoded|
				encoded.drop(1).replace("%2F", "/").replace("%25", "%")
			};

			protect {
				if(compilePath.isNil) {
					temporaryPath = PathName.tmp +/+ ("onda-" ++ UniqueID.next ++ ".onda");
					File.use(temporaryPath, "w", { |file| file.write(source) });
					compilePath = temporaryPath;
				};

				oscFunc = OSCFunc({ |msg, time, addr|
				var rawStr = msg.last.asString;
				var parts = rawStr.split($/);

				if(parts[0].asSymbol == \_onda and: { parts[1].asInteger == 2 }) {
					var replyId = parts[2].asInteger;
					var replyGeneration = parts[3].asInteger;

					if((id == replyId) and: { compileGeneration == replyGeneration }) {
						var success = parts[4].asSymbol != \_fail;
						if (success) {
							var numIns = parts[4].asInteger;
							var cursor = 5;
							var compiledIns = Array.newClear(numIns);
							var compiledSpecs = IdentityDictionary.new;

							numIns.do({ |inputIndex|
								var name = parts[cursor].asSymbol;
								var rateInt = parts[cursor + 1].asInteger;
								var kindInt = parts[cursor + 2].asInteger;
								var hasInit = parts[cursor + 3].asInteger != 0;
								var init = parts[cursor + 4].asFloat;
								var hasSpec = parts[cursor + 5].asInteger != 0;
								var rateSym;
								var meta = (
									kind: case
									{ kindInt == 0 } { \input }
									{ kindInt == 1 } { \param }
									{ kindInt == 2 } { \event }
									{ kindInt == 3 } { \buffer }
									{ \input }
								);

								if (rateInt == 0) {
									rateSym = \audio;
								} {
									rateSym = \control;
								};

								if(hasInit) { meta[\init] = init };
								cursor = cursor + 6;

								if(hasSpec) {
									var minimum = parts[cursor].asFloat;
									var maximum = parts[cursor + 1].asFloat;
									var scale = parts[cursor + 2].asSymbol;
									var hasCurve = parts[cursor + 3].asInteger != 0;
									var curve = parts[cursor + 4].asFloat;
									var hasStep = parts[cursor + 5].asInteger != 0;
									var step = if(hasStep) { parts[cursor + 6].asFloat } { 0.0 };
									var unit = decodeReplyField.(parts[cursor + 7]);
									var warp = if(hasCurve) { curve } { scale };
									var default = if(hasInit) { init } { 0.0 };
									var spec = OndaControlSpec(
										minimum, maximum, warp, step,
										default, unit);

									spec.default = spec.constrain(default);
									meta[\spec] = spec;
									compiledSpecs.put(name, spec);
									cursor = cursor + 8;
								};

								compiledIns[inputIndex] = (
									name: name,
									rate: rateSym,
									meta: meta
								);
							});

							ins = compiledIns;
							outs = parts[cursor].asInteger;
							specs = compiledSpecs;
							all.put(key, this);
							compileSucceeded = true;

							"OndaDef: Compilation of '%' succeeded.".format(key).postln;
						} {
							"OndaDef: Compilation of '%' failed.".format(key).error;
						};

						cond.unhang;
					};
				};
				}, '/done', server.addr);

				server.sendMsg(
					"/cmd", "onda_compile", id, compileGeneration, compilePath);
				cond.hang;
			} {
				if(oscFunc.notNil) { oscFunc.free };
				if(temporaryPath.notNil) {
					if(File.delete(temporaryPath).not) {
						"OndaDef: Could not delete temp file %".format(temporaryPath).warn;
					};
				};
			};

			if(action.notNil.and(compileSucceeded)) {
				action.value(this);
			};
		}
	}

	add { |server, action|
		this.send(server, action);
	}

	free { |server|
		server = server ? Server.default;
		if(server.serverRunning) {
			generation = this.class.nextGenerationFor(key);
			server.sendMsg("/cmd", "onda_free", id, generation);
		};
		all.removeAt(key);
	}

	*free { |key|
		var def = OndaDef.all[key.asSymbol];
		if (def.isOndaDef) {
			def.free;
		} {
			("Invalid OndaDef to free: '" ++ key ++ "'").error;
		}
	}

	*freeAll {
		all.values.copy.do { |def| def.free };
		all.clear;
	}

	asString {
		^(this.class.asString ++ "(" ++ key.asString ++ " : " ++ id.asString ++ ")");
	}

	query {
		("\nKey: " ++ key).postln;
		("Definition id: " ++ id).postln;
		("Generation: " ++ generation).postln;
		("Inputs: " ++ ins).postln;
		("Outputs: " ++ outs).postln;
		("Specs: " ++ specs).postln;
	}

	isOndaDef {
		^true
	}
}

Onda : MultiOutUGen {
	*ar { |def ... args|
		var inputs;
		var inputMap = nil;
		var defKey = def;

		if (def.class == Symbol) {
			def = OndaDef.all[def];
		};

		if (def.isOndaDef.not) {
			"Onda: OndaDef for '%' not found.".format(defKey).warn;
			^Silent.ar(1);
		};

		if (args.size == 1 and: { args[0].isKindOf(Event) }) {
			inputMap = args[0];
		};
		inputs = Array.newClear(def.ins.size);

		def.ins.do { |in, i|
			var name = in[\name];
			var rate = in[\rate];
			var meta = in[\meta];
			var val;

			if (inputMap.notNil) {
				val = inputMap[name];
			} {
				if (i < args.size) {
					val = args[i];
				};
			};

			if (val.isNil) {
				var initVal = meta[\init];
				if (initVal.notNil) {
					val = initVal;
				} {
					"Onda '%': Input '%' missing and no init provided. Using 0.0.".format(def.key, name).warn;
					val = 0.0;
				};
			};

			if (rate == \audio) {
				var converted = false;

				if (val.isUGen) {
					if (val.rate != \audio) {
						val = K2A.ar(val);
						converted = true;
					};
				} {
					val = K2A.ar(val);
					converted = true;
				};

				if (converted) {
					"Onda '%': Converted control input '%' to audio rate.".format(def.key, name).warn;
				};
			};

			if (rate == \control) {
				if (val.isUGen) {
					if (val.rate == \audio ) {
						val = A2K.kr(val);
						"Onda '%': Converted audio input '%' to control rate.".format(def.key, name).warn;
					};
				};
			};

			inputs[i] = val;
		};

		^this.multiNewList(['audio', def.id, def.outs] ++ inputs);
	}

	init { |definitionId, numOutputs ... args|
		inputs = [definitionId] ++ args;
		^this.initOutputs(numOutputs, \audio);
	}
}

+Object {
	isOndaDef {
		^false
	}
}
