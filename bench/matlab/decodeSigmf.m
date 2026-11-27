function count = decodeSigmf(metaFile, beastFile, receiver)
%DECODESIGMF Run a frame receiver on SigMF IQ, writing timestamped Mode S.
% receiver has SampleRate, SamplesPerFrame, and a Decode(iq, seconds)
% function returning a struct array with RawBits, CRCError, and Time.
% Time is the packet time in seconds from the start of the recording.
meta = jsondecode(fileread(metaFile));
% jsondecode renames keys that are MATLAB keywords, including "global".
globalInfo = meta.(matlab.lang.makeValidName('global'));
sampleRate = globalInfo.core_sample_rate;
if sampleRate ~= receiver.SampleRate
    error('adsb:SampleRate', 'Recording is %g Hz; receiver expects %g Hz.', ...
        sampleRate, receiver.SampleRate);
end
if isfield(globalInfo, 'core_num_channels') && globalInfo.core_num_channels ~= 1
    error('adsb:Channels', 'Expected one interleaved complex IQ channel.');
end
switch globalInfo.core_datatype
    case 'cu8'
        precision = 'uint8';
        bytes = 1;
        bias = 127.5;
        scale = 127.5;
    case 'ci8'
        precision = 'int8';
        bytes = 1;
        bias = 0;
        scale = 127;
    case 'ci16_le'
        precision = 'int16';
        bytes = 2;
        bias = 0;
        scale = 32767;
    case 'cf32_le'
        precision = 'single';
        bytes = 4;
        bias = 0;
        scale = 1;
    otherwise
        error('adsb:Datatype', 'Unsupported IQ datatype: %s.', globalInfo.core_datatype);
end

dataFile = regexprep(char(metaFile), '\.sigmf-meta$', '.sigmf-data');
if strcmp(dataFile, char(metaFile))
    error('adsb:Metadata', 'Expected a .sigmf-meta filename.');
end
if isempty(regexp(char(beastFile), '\.beast$', 'once'))
    error('adsb:Output', 'Expected a .beast output filename.');
end
data = dir(dataFile);
if numel(data) ~= 1 || data.isdir || mod(data.bytes, 2 * bytes) ~= 0
    error('adsb:Data', 'Missing IQ file or incomplete complex sample: %s.', dataFile);
end
[input, message] = fopen(dataFile, 'rb', 'ieee-le');
if input < 0
    error('adsb:Input', '%s', message);
end
closeInput = onCleanup(@() fclose(input));
[output, message] = fopen(beastFile, 'wb');
if output < 0
    error('adsb:Output', '%s', message);
end
closeOutput = onCleanup(@() fclose(output));

count = 0;
for first = 0:receiver.SamplesPerFrame:(data.bytes / (2 * bytes) - 1)
    raw = fread(input, [2, receiver.SamplesPerFrame], [precision '=>single']);
    raw = (raw - bias) / scale;
    iq = complex(raw(1, :), raw(2, :)).';
    % The example PHY expects fixed-size input frames. Feed silence for
    % the unused portion of the last frame, and discard detections there.
    stopTime = (first + numel(iq)) / sampleRate;
    iq(end+1:receiver.SamplesPerFrame, 1) = 0;
    packets = receiver.Decode(iq, first / sampleRate);
    for packet = reshape(packets, 1, [])
        if packet.CRCError || ~isfinite(packet.Time) || packet.Time < 0 || packet.Time >= stopTime
            continue
        end
        bits = double(packet.RawBits(:));
        if numel(bits) >= 56 && bits(1) == 0
            type = uint8('2');
            bits = bits(1:56);
        elseif numel(bits) >= 112 && bits(1) == 1
            type = uint8('3');
            bits = bits(1:112);
        else
            error('adsb:Packet', 'Expected 56 or 112 raw Mode S bits.');
        end
        if any(bits ~= 0 & bits ~= 1)
            error('adsb:Packet', 'RawBits must contain binary values.');
        end
        payload = uint8([128 64 32 16 8 4 2 1] * reshape(bits, 8, []));
        ticks = uint64(round(packet.Time * 12e6));
        stamp = uint8(bitand(bitshift(ticks, -(40:-8:0)), uint64(255)));
        body = [stamp uint8(0) payload];
        escaped = repelem(body, 1 + (body == 26));
        frame = [uint8(26) type escaped];
        if fwrite(output, frame, 'uint8') ~= numel(frame)
            error('adsb:Output', 'Failed to write a Beast frame.');
        end
        count = count + 1;
    end
end
end
