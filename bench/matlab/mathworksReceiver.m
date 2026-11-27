function receiver = mathworksReceiver(exampleDir)
%MATHWORKSRECEIVER Adapt the MathWorks ADS-B example PHY to decodeSigmf.
% Invoke from a project session with the example installed locally.
addpath(exampleDir);
settings = load(fullfile(exampleDir, 'defaultinputsADSB.mat'), 'userInput');
[params, source] = helperAdsbConfig(settings.userInput);
release(source);

% The example's captured-signal configuration uses 2.4 MS/s on input.
receiver.SampleRate = 2.4e6;
receiver.SamplesPerFrame = round(params.FrameDuration * receiver.SampleRate);
receiver.Decode = @(iq, time) decode(iq, time, params);
end

function packets = decode(iq, time, params)
[packets, count] = helperAdsbRxPhy(iq, time, params);
packets = packets(1:count);
end
