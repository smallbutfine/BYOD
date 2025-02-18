#include "EnvelopeFilter.h"
#include "../ParameterHelpers.h"
#include "gui/utils/ModulatableSlider.h"

namespace
{
constexpr int mSize = 16;

constexpr float highQ = 5.0f;
constexpr float lowQ = 1.0f / MathConstants<float>::sqrt2;
float getQ (float param01)
{
    return lowQ * std::pow ((highQ / lowQ), param01);
}

const auto speedRange = ParameterHelpers::createNormalisableRange (5.0f, 100.0f, 20.0f);

const String senseTag = "sense";
const String directControlTag = "direct_control";
const String freqModTag = "freq_mod";
} // namespace

EnvelopeFilter::EnvelopeFilter (UndoManager* um) : BaseProcessor ("Envelope Filter", createParameterLayout(), um)
{
    using namespace ParameterHelpers;
    loadParameterPointer (freqParam, vts, "freq");
    loadParameterPointer (resParam, vts, "res");
    loadParameterPointer (speedParam, vts, "speed");
    loadParameterPointer (senseParam, vts, senseTag);
    loadParameterPointer (freqModParam, vts, freqModTag);
    filterTypeParam = vts.getRawParameterValue ("filter_type");
    directControlParam = vts.getRawParameterValue (directControlTag);

    addPopupMenuParameter (directControlTag);

    uiOptions.backgroundColour = Colours::purple.brighter();
    uiOptions.powerColour = Colours::yellow.darker (0.1f);
    uiOptions.info.description = "A envelope filter with lowpass, bandpass, and highpass filter types. Use the right-click menu to control the filter modulation directly";
    uiOptions.info.authors = StringArray { "Jatin Chowdhury" };
}

ParamLayout EnvelopeFilter::createParameterLayout()
{
    using namespace ParameterHelpers;
    auto params = createBaseParams();

    createPercentParameter (params, "res", "Resonance", 0.5f);
    createFreqParameter (params, "freq", "Freq.", 100.0f, 1000.0f, 250.0f, 250.0f);
    createPercentParameter (params, "speed", "Speed", 0.5f);
    createPercentParameter (params, senseTag, "Sensitivity", 0.5f);
    createPercentParameter (params, freqModTag, "Freq. Mod", 0.0f);

    emplace_param<AudioParameterChoice> (params, "filter_type", "Type", StringArray { "Lowpass", "Bandpass", "Highpass" }, 0);

    emplace_param<AudioParameterBool> (params, directControlTag, "Direct Control", false);

    return { params.begin(), params.end() };
}

void EnvelopeFilter::prepare (double sampleRate, int samplesPerBlock)
{
    dsp::ProcessSpec spec { sampleRate, (uint32) samplesPerBlock, 2 };
    auto monoSpec = spec;
    monoSpec.numChannels = 1;

    filter.prepare (spec);
    level.prepare (monoSpec);

    levelBuffer.setSize (1, samplesPerBlock);
}

void EnvelopeFilter::fillLevelBuffer (AudioBuffer<float>& buffer, bool directControlOn)
{
    const auto numChannels = buffer.getNumChannels();
    const auto numSamples = buffer.getNumSamples();

    levelBuffer.setSize (1, numSamples, false, false, true);
    levelBuffer.clear();

    if (directControlOn)
    {
        FloatVectorOperations::fill (levelBuffer.getWritePointer (0), *freqModParam, numSamples);
    }
    else if (numChannels == 1)
    {
        levelBuffer.copyFrom (0, 0, buffer.getReadPointer (0), numSamples);
    }
    else
    {
        levelBuffer.copyFrom (0, 0, buffer.getReadPointer (0), numSamples);

        for (int ch = 1; ch < numChannels; ++ch)
            levelBuffer.addFrom (0, 0, buffer.getReadPointer (ch), numSamples);

        levelBuffer.applyGain (1.0f / (float) numChannels);
    }

    auto speed = speedRange.convertFrom0to1 (1.0f - *speedParam);
    level.setParameters (speed, speed * 4.0f);
    dsp::AudioBlock<float> levelBlock { levelBuffer };
    dsp::ProcessContextReplacing<float> levelCtx { levelBlock };
    level.process (levelCtx);
}

template <chowdsp::StateVariableFilterType FilterType, typename ModFreqFunc>
void processFilter (AudioBuffer<float>& buffer, chowdsp::SVFMultiMode<float>& filter, ModFreqFunc&& getModFreq)
{
    if constexpr (FilterType == chowdsp::StateVariableFilterType::Lowpass)
        filter.setMode (0.0f);
    else if constexpr (FilterType == chowdsp::StateVariableFilterType::Bandpass)
        filter.setMode (0.5f);
    else if constexpr (FilterType == chowdsp::StateVariableFilterType::Highpass)
        filter.setMode (1.0f);

    const auto numChannels = buffer.getNumChannels();
    const auto numSamples = buffer.getNumSamples();

    for (int ch = 0; ch < numChannels; ++ch)
    {
        auto* x = buffer.getWritePointer (ch);

        int i = 0;
        for (int n = 0; n < numSamples - mSize; n += mSize)
        {
            filter.setCutoffFrequency (getModFreq (i));
            for (; i < n + mSize; ++i)
                x[i] = filter.processSample (ch, x[i]);
        }

        // process leftover samples
        filter.setCutoffFrequency (getModFreq (i));
        for (; i < numSamples; ++i)
            x[i] = filter.processSample (ch, x[i]);
    }
}

void EnvelopeFilter::processAudio (AudioBuffer<float>& buffer)
{
    const auto numSamples = buffer.getNumSamples();

    bool directControlOn = *directControlParam == 1.0f;
    fillLevelBuffer (buffer, directControlOn);

    auto filterFreqHz = freqParam->getCurrentValue();
    filter.setQValue (getQ (resParam->getCurrentValue()));

    auto freqModGain = directControlOn ? 10.0f : (20.0f * senseParam->getCurrentValue());
    auto* levelPtr = levelBuffer.getReadPointer (0);
    FloatVectorOperations::clip (levelBuffer.getWritePointer (0), levelPtr, 0.0f, 2.0f, numSamples);

    auto getModFreq = [=] (int i) -> float
    {
        return jlimit (20.0f, 20.0e3f, filterFreqHz + freqModGain * levelPtr[i] * filterFreqHz);
    };

    using SVFType = chowdsp::StateVariableFilterType;
    auto filterType = (int) filterTypeParam->load();
    switch (filterType)
    {
        case 0:
            processFilter<SVFType::Lowpass> (buffer, filter, getModFreq);
            break;

        case 1:
            processFilter<SVFType::Bandpass> (buffer, filter, getModFreq);
            break;

        case 2:
            processFilter<SVFType::Highpass> (buffer, filter, getModFreq);
            break;

        default:
            jassertfalse;
    }

    buffer.applyGain (Decibels::decibelsToGain (-6.0f));
}

bool EnvelopeFilter::getCustomComponents (OwnedArray<Component>& customComps, chowdsp::HostContextProvider& hcp)
{
    using namespace chowdsp::ParamUtils;
    class ControlSlider : public Slider
    {
    public:
        ControlSlider (AudioProcessorValueTreeState& vtState, chowdsp::HostContextProvider& hcp)
            : vts (vtState),
              freqModSlider (*getParameterPointer<chowdsp::FloatParameter*> (vts, freqModTag), hcp),
              sensitivitySlider (*getParameterPointer<chowdsp::FloatParameter*> (vts, senseTag), hcp),
              freqModAttach (vts, freqModTag, freqModSlider),
              senseAttach (vts, senseTag, sensitivitySlider),
              directControlAttach (
                  *vts.getParameter (directControlTag),
                  [this] (float newValue)
                  { updateSliderVisibility (newValue == 1.0f); },
                  vts.undoManager)
        {
            for (auto* s : { &freqModSlider, &sensitivitySlider })
                addChildComponent (s);

            hcp.registerParameterComponent (freqModSlider, freqModSlider.getParameter());
            hcp.registerParameterComponent (sensitivitySlider, sensitivitySlider.getParameter());

            this->setName (senseTag + "__" + freqModTag + "__");
        }

        void colourChanged() override
        {
            for (auto* s : { &freqModSlider, &sensitivitySlider })
            {
                for (auto colourID : { Slider::textBoxOutlineColourId,
                                       Slider::textBoxTextColourId,
                                       Slider::textBoxBackgroundColourId,
                                       Slider::textBoxHighlightColourId,
                                       Slider::thumbColourId })
                {
                    s->setColour (colourID, findColour (colourID, false));
                }
            }
        }

        void updateSliderVisibility (bool directControlOn)
        {
            sensitivitySlider.setVisible (! directControlOn);
            freqModSlider.setVisible (directControlOn);

            setName (vts.getParameter (directControlOn ? freqModTag : senseTag)->name);
            if (auto* parent = getParentComponent())
                parent->repaint();
        }

        void visibilityChanged() override
        {
            updateSliderVisibility (vts.getRawParameterValue (directControlTag)->load() == 1.0f);
        }

        void resized() override
        {
            for (auto* s : { &freqModSlider, &sensitivitySlider })
            {
                s->setSliderStyle (getSliderStyle());
                s->setTextBoxStyle (getTextBoxPosition(), false, getTextBoxWidth(), getTextBoxHeight());
            }

            sensitivitySlider.setBounds (getLocalBounds());
            freqModSlider.setBounds (getLocalBounds());
        }

    private:
        using SliderAttachment = AudioProcessorValueTreeState::SliderAttachment;

        AudioProcessorValueTreeState& vts;
        ModulatableSlider freqModSlider, sensitivitySlider;
        SliderAttachment freqModAttach, senseAttach;
        ParameterAttachment directControlAttach;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ControlSlider)
    };

    customComps.add (std::make_unique<ControlSlider> (vts, hcp));

    return false;
}
