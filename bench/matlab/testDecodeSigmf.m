function testDecodeSigmf()
% Exercise the shared runner without the Communications Toolbox.
folder = tempname;
mkdir(folder);
cleanup = onCleanup(@() rmdir(folder, 's'));
base = fullfile(folder, 'pilot');
metaFile = [base '.sigmf-meta'];
receiver = struct('SampleRate', 2.4e6, 'SamplesPerFrame', 2, 'Decode', @fixture);
output = [base '.beast'];
% DF17, Beast timestamp 5, escaped payload byte 0x1a.
expected = [26 51 0 0 0 0 0 5 0 141 26 26 zeros(1, 12)];
formats = {
    'cu8', 'uint8', 0, 255, 128;
    'ci8', 'int8', -127, 127, 0;
    'ci16_le', 'int16', -32767, 32767, 0;
    'cf32_le', 'single', -1, 1, 0
};
for row = 1:size(formats, 1)
    [format, precision, lo, hi, mid] = formats{row, :};
    fid = fopen(metaFile, 'w');
    fprintf(fid, '{"global":{"core:datatype":"%s","core:sample_rate":2400000}}', format);
    fclose(fid);
    fid = fopen([base '.sigmf-data'], 'wb', 'ieee-le');
    fwrite(fid, [lo hi hi lo mid mid], precision);
    fclose(fid);

    assert(decodeSigmf(metaFile, output, receiver) == 1);
    fid = fopen(output, 'rb');
    actual = fread(fid).';
    fclose(fid);
    assert(isequal(actual, expected));
end
end

function packets = fixture(iq, time)
bits = reshape(dec2bin([141 26 zeros(1, 12)], 8).' - '0', [], 1);
packet = struct('RawBits', bits, 'CRCError', false, 'Time', 1 / 2.4e6);
if time == 0
    assert(isequal(iq, [-1+1i; 1-1i]));
    rejected = packet;
    rejected.CRCError = true;
    packets = [packet rejected];
else
    assert(abs(iq(1)) < 0.01 && iq(2) == 0);
    packet.Time = 3 / 2.4e6;
    packets = packet;
end
end
