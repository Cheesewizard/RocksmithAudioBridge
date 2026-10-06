using System;
using System.Diagnostics;
using System.IO;
using System.IO.MemoryMappedFiles;
using System.Runtime.InteropServices;
using System.Threading;

namespace RSModsPlus.MachineLearning
{
	public static class MlService
	{
		private const int RING_SAMPLES = 1 << 17;
		private const int HEADER_BYTES = 64;
		private const uint AUDIO_MAGIC = 0x4C4D5352;
		private const uint RESULT_MAGIC = 0x46535352;

		public static void Run(int parentProcessId, Action<string> log, string channelSuffix = "")
		{
			if (parentProcessId <= 0) throw new ArgumentOutOfRangeException(nameof(parentProcessId));
			if (log == null) throw new ArgumentNullException(nameof(log));
			if (channelSuffix == null || (channelSuffix.Length != 0 && !channelSuffix.StartsWith(".Test", StringComparison.Ordinal))) throw new ArgumentException("Only isolated test channel suffixes are allowed.", nameof(channelSuffix));
			using (var parent = Process.GetProcessById(parentProcessId))
			using (var writerMutex = new Mutex(false, "Local\\RSModsPlus.MlWriter" + channelSuffix))
			{
				var ownsMutex = false;
				try
				{
					try { ownsMutex = writerMutex.WaitOne(0); }
					catch (AbandonedMutexException) { ownsMutex = true; }
					if (!ownsMutex) throw new InvalidOperationException("An ML result writer is already running.");
					var transform = new CqtTransform();
					using (var model = new FretNetSession())
					using (var bassModel = new FretNetSession(bass: true))
					{
						var warmup = transform.ComputeLatest(new float[57600], 48000, out var warmupCenter);
						model.Predict(warmup);
						bassModel.Predict(warmup);
						log("Embedded FretNet loaded, guitar sha256=" + EmbeddedModel.GetSha256()
							+ ", bass sha256=" + EmbeddedModel.GetSha256(bass: true) + ". Waiting for game audio.");
						using (var audio = OpenAudio(parent, channelSuffix))
						{
							if (audio == null) return;
							using (var input = audio.CreateViewAccessor(0, HEADER_BYTES + RING_SAMPLES * sizeof(float), MemoryMappedFileAccess.Read))
							using (var result = MemoryMappedFile.CreateOrOpen("Local\\RSModsPlus.MlStringFret.v2" + channelSuffix, 112))
							using (var output = result.CreateViewAccessor(0, 112))
							{
								if (input.ReadUInt32(0) != AUDIO_MAGIC || input.ReadUInt32(4) != 2) throw new InvalidDataException("Game audio mapping version mismatch.");
								output.Write(0, RESULT_MAGIC);
								output.Write(4, 2u);
								ProcessAudio(parent, input, output, model, bassModel, transform, log, channelSuffix);
							}
						}
					}
				}
				finally
				{
					if (ownsMutex) writerMutex.ReleaseMutex();
				}
			}
		}

		private static MemoryMappedFile OpenAudio(Process parent, string suffix)
		{
			while (!parent.HasExited)
			{
				try { return MemoryMappedFile.OpenExisting("Local\\RSModsPlus.MlAudio.v2" + suffix, MemoryMappedFileRights.Read); }
				catch (FileNotFoundException) { Thread.Sleep(100); }
			}
			return null;
		}

		private static void ProcessAudio(Process parent, MemoryMappedViewAccessor input, MemoryMappedViewAccessor output,
			FretNetSession model, FretNetSession bassModel, CqtTransform transform, Action<string> log, string suffix)
		{
			var clock = Stopwatch.StartNew();
			var nextChartPoll = 0L;
			var nextLog = 0L;
			// { expectedString, expectedMidi, instrument (0 guitar, 1 bass) }
			var expected = new[] { -1, -1, 0 };
			var previousInstrument = 0;
			var chartUnavailable = false;
			var previousRate = 0;
			var previousShift = 0;
			var lastEnd = 0UL;
			var validStart = 0UL;
			var sequence = 0u;
			var samples = new float[0];
			while (!parent.HasExited)
			{
				if (clock.ElapsedMilliseconds >= nextChartPoll)
				{
					nextChartPoll = clock.ElapsedMilliseconds + 250;
					try
					{
						expected = ChartExpectationReader.Read(input);
						if (chartUnavailable) log("Chart expectation is available again.");
						chartUnavailable = false;
					}
					catch (Exception exception) when (exception is IOException || exception is TimeoutException || exception is AggregateException || exception is ArgumentException)
					{
						// Keep the last known instrument: only the expected note is unknown.
						expected = new[] { -1, -1, expected[2] };
						if (!chartUnavailable) log("Chart expectation unavailable; publishing unconstrained model evidence: " + exception.Message);
						chartUnavailable = true;
					}
				}
				var instrument = expected[2];
				if (instrument != previousInstrument)
				{
					previousInstrument = instrument;
					log("Arrangement instrument: " + (instrument == 1 ? "bass, using the bass model." : "guitar, using the guitar model."));
				}
				var rate = input.ReadInt32(8);
				var shift = input.ReadInt32(12);
				var end = input.ReadUInt64(16);
				Thread.MemoryBarrier();
				if (rate == 0) { Thread.Sleep(2); continue; }
				if (rate < 8000 || rate > 96000) throw new InvalidDataException("Unsupported game audio rate: " + rate);
				if (rate != previousRate || shift != previousShift || end < lastEnd)
				{
					validStart = lastEnd = end;
					previousRate = rate;
					previousShift = shift;
					Publish(output, new NoteEvidence(), shift, end, rate, instrument, ref sequence);
					log("Audio timeline reset: rate=" + rate + ", shift=" + shift);
				}
				var window = Math.Min((int)(1.2 * rate), RING_SAMPLES - 1);
				var hop = (ulong)Math.Max(1, Math.Round(CqtTransform.HOP_LENGTH * (double)rate / CqtTransform.SAMPLE_RATE));
				if (end < (ulong)window || end - lastEnd < hop) { Thread.Sleep(2); continue; }
				if (samples.Length != window) samples = new float[window];
				var start = end - (ulong)window;
				var ringStart = (int)(start & (RING_SAMPLES - 1));
				var firstCount = Math.Min(window, RING_SAMPLES - ringStart);
				input.ReadArray(HEADER_BYTES + ringStart * sizeof(float), samples, 0, firstCount);
				if (firstCount < window) input.ReadArray(HEADER_BYTES, samples, firstCount, window - firstCount);
				Thread.MemoryBarrier();
				var afterCopy = input.ReadUInt64(16);
				if (afterCopy < end || afterCopy - start > RING_SAMPLES) { log("Discarded overwritten audio snapshot."); continue; }
				lastEnd = end;
				for (var index = 0; index < samples.Length; index++)
				{
					if (float.IsNaN(samples[index]) || float.IsInfinity(samples[index])) throw new InvalidDataException("Game audio contains a nonfinite sample.");
				}
				var power = 0.0;
				var tail = Math.Min(window, rate / 4);
				for (var index = window - tail; index < window; index++) power += samples[index] * samples[index];
				var evidence = new NoteEvidence();
				var analyzedSample = end;
				var started = clock.Elapsed.TotalMilliseconds;
				if (Math.Sqrt(power / tail) >= 3e-4)
				{
					var features = transform.ComputeLatest(samples, rate, out var center);
					analyzedSample = start + (ulong)center;
					if (analyzedSample < validStart) continue;
					var bass = instrument == 1;
					evidence = NoteEvidence.Decode((bass ? bassModel : model).Predict(features)[0], expected[0], expected[1], bass);
				}
				Thread.MemoryBarrier();
				var currentEnd = input.ReadUInt64(16);
				if (input.ReadInt32(8) != rate || input.ReadInt32(12) != shift || currentEnd < end) continue;
				Publish(output, evidence, shift, analyzedSample, rate, instrument, ref sequence);
				if (clock.ElapsedMilliseconds >= nextLog)
				{
					nextLog = clock.ElapsedMilliseconds + 2000;
					log("Published sample=" + analyzedSample + " analysisMs=" + (clock.Elapsed.TotalMilliseconds - started).ToString("F2")
						+ " frets=" + string.Join(",", evidence.Frets));
				}
			}
		}

		private static void Publish(MemoryMappedViewAccessor output, NoteEvidence evidence, int shift, ulong sample, int rate, int instrument, ref uint sequence)
		{
			output.Write(8, ++sequence);
			Thread.MemoryBarrier();
			output.Write(12, shift);
			for (var guitarString = 0; guitarString < 6; guitarString++)
			{
				var fret = evidence.Frets[guitarString];
				output.Write(16 + guitarString * 4, fret);
				output.Write(40 + guitarString * 4, fret < 0 ? -1 : fret - shift);
				output.Write(64 + guitarString * 4, evidence.Confidence[guitarString]);
			}
			output.Write(88, GetTickCount64());
			output.Write(96, sample);
			output.Write(104, rate);
			// Which model produced the frets (0 guitar, 1 bass): the host decodes with that instrument's
			// open strings and refuses a read that does not match the loaded arrangement.
			output.Write(108, instrument);
			Thread.MemoryBarrier();
			output.Write(8, ++sequence);
		}

		[DllImport("kernel32.dll")]
		private static extern ulong GetTickCount64();
	}
}
