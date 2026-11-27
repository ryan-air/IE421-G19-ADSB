function [receiver, params] = mathworksReceiver(exampleDir, candidateCapacity)
%MATHWORKSRECEIVER Adapt the MathWorks ADS-B example PHY to decodeSigmf.
% Invoke from a project session with the example installed locally.
% Pass a capacity only for a separately labeled high-density run.
addpath(exampleDir);
settings = load(fullfile(exampleDir, 'defaultinputsADSB.mat'), 'userInput');
[params, source] = helperAdsbConfig(settings.userInput);
release(source);
if nargin == 2
    validateattributes(candidateCapacity, {'numeric'}, {'scalar', 'integer', 'positive'});
    params.MaxNumPacketsInFrame = candidateCapacity;
end

receiver.SampleRate = params.FrontEndSampleRate;
receiver.SamplesPerFrame = params.SamplesPerFrame;
receiver.Decode = @(iq, time) decode(iq, time, params);
end

function packets = decode(iq, time, params)
% The example's synchronizer and interpolator retain prior frame samples.
if time == 0
    clear helperAdsbRxPhy helperAdsbRxPhyInterpolator helperAdsbRxPhyBitParser
end
[packets, count] = helperAdsbRxPhy(iq, time, params);
packets = packets(1:count);
end
