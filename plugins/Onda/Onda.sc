OndaDef {
	classvar <all, <definitionIds, <generations;
	classvar nextDefinitionId;

	var <key;
	var <id;
	var <generation;
	var <>numAllocate;
	var <source;
	var sourcePath;

	var <ins;
	var <outs;

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

	send { |server, action, numAllocate = 32|
		var allocateCount = numAllocate.asInteger;
		var compileGeneration;

		server = server ? Server.default;
		if(source.isNil) {
			"OndaDef '%': No valid source to send.".format(key).error;
			^this
		};
		if((allocateCount < 1) or: { allocateCount > 4096 }) {
			"OndaDef '%': numAllocate must be between 1 and 4096.".format(key).error;
			^this
		};
		if(server.serverRunning.not) {
			"OndaDef: Server not running. Definition not sent.".warn;
			^this
		};

		compileGeneration = this.class.nextGenerationFor(key);
		generation = compileGeneration;
		this.numAllocate_(allocateCount);

		forkIfNeeded {
			var cond = Condition(false);
			var compilePath = sourcePath;
			var temporaryPath;
			var compileSucceeded = false;
			var oscFunc;

			protect {
				if(compilePath.isNil) {
					temporaryPath = PathName.tmp +/+ ("onda-" ++ UniqueID.next ++ ".onda");
					File.use(temporaryPath, "w", { |file| file.write(source) });
					compilePath = temporaryPath;
				};

				oscFunc = OSCFunc({ |msg, time, addr|
				var rawStr = msg.last.asString;
				var parts = rawStr.split($/);

				if(parts[0].asSymbol == \_onda) {
					var replyId = parts[1].asInteger;
					var replyGeneration = parts[2].asInteger;

					if((id == replyId) and: { compileGeneration == replyGeneration }) {
						var success = parts[3].asSymbol != \_fail;
						if (success) {
							var numIns = parts[3].asInteger;
							var cursor = 4;

							ins = Array.newClear(numIns);

							numIns.do({ |inputIndex|
								var name = parts[cursor].asSymbol;
								var rateInt = parts[cursor + 1].asInteger;
								var kindInt = parts[cursor + 2].asInteger;
								var hasInit = parts[cursor + 3].asInteger != 0;
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

								if(hasInit) { meta[\init] = parts[cursor + 4].asFloat };
								cursor = cursor + 5;

								ins[inputIndex] = (
									name: name,
									rate: rateSym,
									meta: meta
								);
							});

							outs = parts.last.asInteger;
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
					"/cmd", "onda_compile", id, compileGeneration, allocateCount, compilePath);
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

	add { |server, action, numAllocate = 32|
		this.send(server, action, numAllocate);
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
