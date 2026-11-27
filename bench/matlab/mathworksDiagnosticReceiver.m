function receiver = mathworksDiagnosticReceiver(exampleDir, candidateFile)
%MATHWORKSDIAGNOSTICRECEIVER Trace accepted preambles and their parser results.
% One receiver per recording. Keep it alive until decodeSigmf returns.
[receiver, params] = mathworksReceiver(exampleDir);
[log, message] = fopen(candidateFile, 'w');
if log < 0
    error('adsb:DiagnosticOutput', '%s', message);
end
closeLog = onCleanup(@() fclose(log));
fprintf(log, 'time_s,df,crc_error,hex\n');
synchronizer = helperAdsbRxPhySync(params);
receiver.Decode = @decode;
receiver.CloseDiagnosticLog = closeLog;

    function packets = decode(iq, time)
        if time == 0
            clear helperAdsbRxPhyInterpolator helperAdsbRxPhyBitParser
            release(synchronizer);
        end
        samples = helperAdsbRxPhyInterpolator(iq, params);
        [candidates, count, starts] = synchronizer(abs(samples).^2);
        if count > size(candidates, 2)
            error('adsb:CandidateOverflow', ...
                'MathWorks accepted %d preambles at %.6f s with capacity %d.', ...
                count, time, size(candidates, 2));
        end
        [packets, count] = helperAdsbRxPhyBitParser(candidates, count, starts, time, params);
        packets = packets(1:count);
        for packet = reshape(packets, 1, [])
            bits = double(packet.RawBits(:));
            if packet.DF < 16
                bits = bits(1:56);
            end
            payload = [128 64 32 16 8 4 2 1] * reshape(bits, 8, []);
            fprintf(log, '%.12f,%d,%d,%s\n', packet.Time, packet.DF, packet.CRCError, ...
                sprintf('%02X', payload));
        end
    end
end
