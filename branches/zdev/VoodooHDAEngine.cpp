#include "License.h"

#include "VoodooHDAEngine.h"
#include "VoodooHDADevice.h"
#include "Common.h"
#include "Verbs.h"
#include "OssCompat.h"
#include "Tables.h"

#include <IOKit/audio/IOAudioDefines.h>
#include <IOKit/audio/IOAudioPort.h>
#include <IOKit/audio/IOAudioSelectorControl.h>
#include <IOKit/audio/IOAudioLevelControl.h>
#include <IOKit/audio/IOAudioToggleControl.h>
#include <IOKit/pci/IOPCIDevice.h>

#ifdef TIGER
#include "TigerAdditionals.h"
#endif

#define super IOAudioEngine
OSDefineMetaClassAndStructors(VoodooHDAEngine, IOAudioEngine)

#define SAMPLE_CHANNELS		2	// forced stereo quirk is always enabled

#define SAMPLE_OFFSET		64	// note: these values definitely need to be tweaked
#define SAMPLE_LATENCY		32

extern const char * const gDeviceTypes[], * const gConnTypes[];

#define kVoodooHDAPortSubTypeBase		'voo\x40'
#define VOODOO_OSS_TO_SUBTYPE(type)		(kVoodooHDAPortSubTypeBase + 1 + type)
#define VOODOO_SUBTYPE_TO_OSS(type)		(type - 1 - kVoodooHDAPortSubTypeBase)

const char * const gOssDeviceTypes[SOUND_MIXER_NRDEVICES] = {
	"Master", "Bass", "Treble", "Synthesizer", "PCM", "Speaker", "Line-in", "Microphone",
	"CD", "Input mix", "Alternate PCM", "Recording level", "Input gain", "Output gain",
	"Line #1", "Line #2", "Line #3", "Digital #1", "Digital #2", "Digital #3",
	"Phone input", "Phone output", "Video", "Radio", "Microphone #2"
};

/******************************************************************************************/
/******************************************************************************************/

#define logMsg(fmt, args...)	if(mVerbose>0)\
		messageHandler(kVoodooHDAMessageTypeGeneral, fmt, ##args)
#define errorMsg(fmt, args...)	messageHandler(kVoodooHDAMessageTypeError, fmt, ##args)
#define dumpMsg(fmt, args...)	messageHandler(kVoodooHDAMessageTypeDump, fmt, ##args)

void VoodooHDAEngine::messageHandler(UInt32 type, const char *format, ...)
{
	va_list args;
	va_start(args, format);
	if (mDevice)
		mDevice->messageHandler(type, format, args);
	else if (mVerbose >= 1)
		vprintf(format, args);
	va_end(args);
}

/******************************************************************************************/
/******************************************************************************************/

bool VoodooHDAEngine::initWithChannel(Channel *channel)
{
	bool result = false;

//	logMsg("VoodooHDAEngine[%p]::init\n", this);

	if (!channel || !super::init(NULL))
		goto done;

	mChannel = channel;
	mActiveOssDev = -1;

	result = true;
done:
	return result;
}

void VoodooHDAEngine::free()
{
//	logMsg("VoodooHDAEngine[%p]::free\n", this);

	RELEASE(mStream);

	RELEASE(mSelControl);

#if 0
	RELEASE(mPort);
#endif

	mDevice = NULL;

	super::free();
}

static
UInt32 pinConfigToSelection(UInt32 pinConfig)
{
	switch (pinConfig & HDA_CONFIG_DEFAULTCONF_DEVICE_MASK) {
		case HDA_CONFIG_DEFAULTCONF_DEVICE_LINE_OUT:
		case HDA_CONFIG_DEFAULTCONF_DEVICE_LINE_IN:
			return kIOAudioSelectorControlSelectionValueLine;
		case HDA_CONFIG_DEFAULTCONF_DEVICE_SPEAKER:
			return kIOAudioSelectorControlSelectionValueExternalSpeaker;
		case HDA_CONFIG_DEFAULTCONF_DEVICE_HP_OUT:
			return kIOAudioSelectorControlSelectionValueHeadphones;
		case HDA_CONFIG_DEFAULTCONF_DEVICE_CD:
			return kIOAudioSelectorControlSelectionValueCD;
		case HDA_CONFIG_DEFAULTCONF_DEVICE_SPDIF_OUT:
		case HDA_CONFIG_DEFAULTCONF_DEVICE_DIGITAL_OTHER_OUT:
		case HDA_CONFIG_DEFAULTCONF_DEVICE_SPDIF_IN:
		case HDA_CONFIG_DEFAULTCONF_DEVICE_DIGITAL_OTHER_IN:
			return kIOAudioSelectorControlSelectionValueSPDIF;
		case HDA_CONFIG_DEFAULTCONF_DEVICE_MIC_IN:
			return kIOAudioSelectorControlSelectionValueExternalMicrophone;
		default:
			return kIOAudioSelectorControlSelectionValueNone;
	}
}

const char *VoodooHDAEngine::getPortName()
{
	UInt32 numDacs;
	nid_t dacNid, outputNid;
	Widget *widget;
	//UInt32 config;
//	const char *devType, *connType;
	//char buf[64]; // = "Unknown";
	AudioAssoc *assoc;

	if (mPortName)
		return mPortName;

	mDevice->lock(__FUNCTION__);

	for (numDacs = 0; (numDacs < 16) && (mChannel->io[numDacs] != -1); numDacs++){
		//Slice - to trace
/*		if (mVerbose > 2) {
			logMsg(" io[%d] in assoc %d = %d\n", (int)numDacs, (int)mChannel->assocNum, (int)mChannel->io[numDacs]);
		}*/
	}
	if (numDacs == 0)
		goto done;
	if (numDacs > 1){
			mPortName = "Complex output";
		goto done;
	}

	dacNid = mChannel->io[0];

	assoc = &mChannel->funcGroup->audio.assocs[mChannel->assocNum];
	outputNid = -1;
	for (int n = 0; (n < 16) && assoc->dacs[n]; n++)
		if (assoc->dacs[n] == dacNid)
			outputNid = assoc->pins[n];
	if (outputNid == -1)
		goto done;

	widget = mDevice->widgetGet(mChannel->funcGroup, outputNid);
	if (!widget)
		goto done;

	//config = widget->pin.config;
	//Slice - advanced PinName

	mPortName = &widget->name[5];
	mPortType = pinConfigToSelection(widget->pin.config);
done:
	mDevice->unlock(__FUNCTION__);

	if (!mPortName)
		mPortName = "Not connected";
	if (!mPortType)
		mPortType = kIOAudioSelectorControlSelectionValueNone;

	return mPortName;
}

const char *VoodooHDAEngine::getDescription(char* callerBuffer, unsigned length)
{
	if (!callerBuffer)
		return 0;
	PcmDevice *pcmDevice = mChannel->pcmDevice;
	snprintf(callerBuffer, length, "%s PCM #%d", (pcmDevice->digital ? "Digital" : "Analog"),
			 pcmDevice->index);
	return callerBuffer;
}

void VoodooHDAEngine::identifyPaths()
{
	IOAudioStreamDirection direction = getEngineDirection();
	FunctionGroup *funcGroup = mChannel->funcGroup;

	for (int i = funcGroup->startNode; i < funcGroup->endNode; i++) {
		Widget *widget;
		UInt32 config;
		const char *devType, *connType;

		widget = mDevice->widgetGet(funcGroup, i);
		if (!widget || widget->enable == 0)
			continue;
		if (((direction == kIOAudioStreamDirectionOutput) &&
				(widget->type != HDA_PARAM_AUDIO_WIDGET_CAP_TYPE_PIN_COMPLEX)) ||
				((direction == kIOAudioStreamDirectionInput) &&
				(widget->type != HDA_PARAM_AUDIO_WIDGET_CAP_TYPE_AUDIO_INPUT)))
			continue;
		if (widget->bindAssoc != mChannel->assocNum)
			continue;
		config = widget->pin.config;
		devType = gDeviceTypes[HDA_CONFIG_DEFAULTCONF_DEVICE(config)];
		connType = gConnTypes[HDA_CONFIG_DEFAULTCONF_CONNECTIVITY(config)];
		if (mVerbose > 3) {
			logMsg("[nid %d] devType = %s, connType = %s\n", i, devType, connType);
		}
	}
}

UInt32 VoodooHDAEngine::getNumCtls(UInt32 dev)
{
	UInt32 numCtls = 0;
	AudioControl *control;

	for (int i = 0; (control = mDevice->audioCtlEach(mChannel->funcGroup, &i)); ) {
		if ((control->enable == 0) || !(control->ossmask & (1 << dev)))
			continue;
		if (!((control->widget->bindAssoc == mChannel->assocNum) || (control->widget->bindAssoc == -2)))
			continue;
		numCtls++;
	}

	return numCtls;
}

UInt64 VoodooHDAEngine::getMinMaxDb(UInt32 mask)
{
	AudioControl *control;
	IOFixed minDb, maxDb;

	minDb = ~0L;
	maxDb = ~0L;

	// xxx: we currently use the values from the first found control (ie. amplifier settings)

	for (int i = 0; (control = mDevice->audioCtlEach(mChannel->funcGroup, &i)); ) {
		if ((control->enable == 0) || !(control->ossmask & mask)) //(1 << dev)))
			continue;
		if (!((control->widget->bindAssoc == mChannel->assocNum) || (control->widget->bindAssoc == -2)))
			continue;
		if (control->step <= 0)
			continue;
		minDb = ((0 - control->offset) * (control->size + 1) / 4) << 16;
		maxDb = ((control->step - control->offset) * (control->size + 1) / 4) << 16;
		break;
	}

	return ((UInt64) minDb << 32) | maxDb;
}

bool VoodooHDAEngine::validateOssDev(int ossDev)
{
	return ((ossDev >= 0) && (ossDev < SOUND_MIXER_NRDEVICES));
}

const char *VoodooHDAEngine::getOssDevName(int ossDev)
{
	if (validateOssDev(ossDev))
		return gOssDeviceTypes[ossDev];
	else
		return "invalid";
}

void VoodooHDAEngine::setActiveOssDev(int ossDev)
{
	logMsg("setting active OSS device: %d (%s)\n", ossDev, getOssDevName(ossDev));
	ASSERT(validateOssDev(ossDev));
	mActiveOssDev = ossDev;
}

int VoodooHDAEngine::getActiveOssDev()
{
	int ossDev = mActiveOssDev;
	logMsg("active OSS device: %d (%s)\n", ossDev, getOssDevName(ossDev));
	ASSERT(validateOssDev(ossDev));
	return ossDev;
}

bool VoodooHDAEngine::initHardware(IOService *provider)
{
	bool result = false;
#if 0
	const char *portName;
	UInt32 portType;
	UInt32 subType;
	IOReturn ret;
#endif

//	logMsg("VoodooHDAEngine[%p]::initHardware\n", this);

	if (!super::initHardware(provider)) {
		errorMsg("error: IOAudioEngine::initHardware failed\n");
		goto done;
	}
	mDevice = OSDynamicCast(VoodooHDADevice, provider);
	ASSERT(mDevice);

	mVerbose = mDevice->mVerbose;
#if 0
	identifyPaths();
#endif
	getPortName();

	//logMsg("setDesc portName = %s\n", mPortName);
	setDescription(mPortName);

#if 0
	// xxx: there must be some way to get port name to appear in the "type" column on the sound
	// preference pane - this used to be the "port" column but apparently this is no longer the
	// case in recent releases of os x
	/*
	 * IOAudioPort is listed as obsolete in Apple's Audio Device Driver Guide dated 2009-03-04.
	 *    Setting of the type column is done via Selector Audio Controls
	 *    -- Zenith432 2013-01-24.
	 */

	portName = mPortName;
	ASSERT(portName);

	if (getEngineDirection() == kIOAudioStreamDirectionOutput) {
		portType = kIOAudioPortTypeOutput;
		// TODO: subType
		subType = kIOAudioOutputPortSubTypeInternalSpeaker;
	}
	else {
		portType = kIOAudioPortTypeInput;
		// TODO: subType
		subType =  kIOAudioInputPortSubTypeInternalMicrophone;
	}

	//logMsg("setDesc portType = %4c subType = %4c\n", portType, subType);

	mPort = IOAudioPort::withAttributes(portType, portName, subType);
	if (!mPort) {
		errorMsg("error: IOAudioPort::withAttributes failed\n");
		goto done;
	}
	ret = mDevice->attachAudioPort(mPort, this, NULL);
	if (ret != kIOReturnSuccess) {
		errorMsg("error: attachAudioPort failed\n");
		goto done;
	}
#endif

	setSampleOffset(SAMPLE_OFFSET);
	setSampleLatency(SAMPLE_LATENCY);

	if (!createAudioStream()) {
		errorMsg("error: createAudioStream failed\n");
		goto done;
	}
	emptyStream = true;
	if (!createAudioControls()) {
		errorMsg("error: createAudioControls failed\n");
		goto done;
	}
	mChannel->vectorize  = mDevice->vectorize;
	mChannel->noiseLevel = mDevice->noiseLevel;
	mChannel->useStereo  = mDevice->useStereo;
	mChannel->StereoBase = mDevice->StereoBase;

	result = true;
done:
	if (!result)
		stop(provider);

	return result;
}

bool VoodooHDAEngine::createAudioStream()
{
	bool result = false;
	IOAudioStreamDirection direction;
	IOAudioSampleRate minSampleRate, maxSampleRate;
	UInt8 *sampleBuffer;
	UInt32 channels;

	ASSERT(!mStream);

//	logMsg("VoodooHDAEngine[%p]::createAudioStream\n", this);

//	logMsg("recDevMask: 0x%lx, devMask: 0x%lx\n", mChannel->pcmDevice->recDevMask,
//			mChannel->pcmDevice->devMask);

	direction = getEngineDirection();

//	logMsg("formats: ");
//	for (UInt32 n = 0; (n < 8) && mChannel->formats[n]; n++)
//		logMsg("0x%lx ", mChannel->formats[n]);
//	logMsg("\n");

	if (!HDA_PARAM_SUPP_STREAM_FORMATS_PCM(mChannel->supStreamFormats)) {
		errorMsg("error: channel doesn't support PCM stream format\n");
		goto done;
	}

//	logMsg("sample rates: ");
//	for (UInt32 n = 0; (n < 16) && mChannel->pcmRates[n]; n++)
//		logMsg("%ld ", mChannel->pcmRates[n]);
//	logMsg("(min: %ld, max: %ld)\n", mChannel->caps.minSpeed, mChannel->caps.maxSpeed);

//	ASSERT(mChannel->caps.minSpeed);
//	ASSERT(mChannel->caps.maxSpeed);
//	ASSERT(mChannel->caps.minSpeed <= mChannel->caps.maxSpeed);

	minSampleRate.whole = mChannel->caps.minSpeed;
	minSampleRate.fraction = 0;
	maxSampleRate.whole = mChannel->caps.maxSpeed;
	maxSampleRate.fraction = 0;
	channels = mChannel->caps.channels;
	logMsg("(min: %ld, max: %ld) channels=%d\n", (long int)mChannel->caps.minSpeed, (long int)mChannel->caps.maxSpeed, (int)channels);
	sampleBuffer = (UInt8 *) mChannel->buffer->virtAddr;
	mBufferSize = HDA_BUFSZ_MAX; // hardcoded in pcmAttach()
    if (!createAudioStream(direction, sampleBuffer, mBufferSize, mChannel->pcmRates,
                           mChannel->supPcmSizeRates, mChannel->supStreamFormats, channels)) {
		errorMsg("error: createAudioStream failed channels=%d\n", (int)channels);
		goto done;
	}
#if 0
	if (HDA_PARAM_SUPP_STREAM_FORMATS_AC3(mChannel->supStreamFormats)) {
//		logMsg("adding AC3 audio stream\n");
		if (!createAudioStream(direction, sampleBuffer, mBufferSize, minSampleRate, maxSampleRate,
							   mChannel->supPcmSizeRates, kIOAudioStreamSampleFormatAC3, channels)) {
			errorMsg("error: createAudioStream AC3 failed\n");
		}
	}
#endif
	result = true;
done:
	return result;
}

bool VoodooHDAEngine::createAudioStream(IOAudioStreamDirection direction, void *sampleBuffer,
		UInt32 sampleBufferSize, UInt32 *pcmRates,
		UInt32 supPcmSizeRates, UInt32 supStreamFormats, UInt32 channels)
{
	bool result = false;
	UInt32 defaultSampleRate = 0U;

	IOAudioStreamFormat format = {
		channels,										// number of channels
		0,                                              // sample format (to be filled in)
		kIOAudioStreamNumericRepresentationSignedInt,	// numeric format
		0,												// bit depth (to be filled in)
		0,												// bit width (to be filled in)
		kIOAudioStreamAlignmentLowByte,					// low byte aligned
		kIOAudioStreamByteOrderLittleEndian,			// little endian
		true,											// format is mixable
		0												// driver-defined tag
	};

    IOAudioStreamFormatExtension formatEx = {
		kFormatExtensionCurrentVersion,					// version
		0,                                              // flags
		0,											    // frames per packet (to be filled in)
		0												// bytes per packet (to be filled in)
	};

    IOAudioSampleRate sampleRate = {
        0,
        0
    };

	ASSERT(!mStream);

//	logMsg("VoodooHDAEngine[%p]::createAudioStream(%d, %p, %ld)\n", this, direction, sampleBuffer,
//			sampleBufferSize);

	mStream = new IOAudioStream;
	if (!mStream->initWithAudioEngine(this, direction, 1)) {
		errorMsg("error: IOAudioStream::initWithAudioEngine failed\n");
		goto done;
	}

	mStream->setSampleBuffer(sampleBuffer, sampleBufferSize); // also creates mix buffer

    for(int i = 0; pcmRates[i]; i++) {
        sampleRate.whole = pcmRates[i];
		if (sampleRate.whole <= 48000U && defaultSampleRate < sampleRate.whole)
			defaultSampleRate = sampleRate.whole;
        if(HDA_PARAM_SUPP_STREAM_FORMATS_AC3(supStreamFormats)) {
            format.fBitDepth = 16;
            format.fBitWidth = 16;
            format.fSampleFormat = kIOAudioStreamSampleFormat1937AC3;
            formatEx.fFramesPerPacket = 1536;
            formatEx.fBytesPerPacket = formatEx.fFramesPerPacket * format.fNumChannels * (format.fBitWidth / 8);
            format.fIsMixable = false;
            mStream->addAvailableFormat(&format, &formatEx, &sampleRate, &sampleRate);
        }
        format.fSampleFormat = kIOAudioStreamSampleFormatLinearPCM;
        formatEx.fFramesPerPacket = 1;
		format.fIsMixable = true;
	if (HDA_PARAM_SUPP_PCM_SIZE_RATE_16BIT(supPcmSizeRates)) {
		format.fBitDepth = 16;
		format.fBitWidth = 16;
            formatEx.fBytesPerPacket = format.fNumChannels * (format.fBitWidth / 8);
            mStream->addAvailableFormat(&format, &formatEx, &sampleRate, &sampleRate);
	}
	if (HDA_PARAM_SUPP_PCM_SIZE_RATE_24BIT(supPcmSizeRates)) {
		format.fBitDepth = 24;
		format.fBitWidth = 32;
            formatEx.fBytesPerPacket = format.fNumChannels * (format.fBitWidth / 8);
            mStream->addAvailableFormat(&format, &formatEx, &sampleRate, &sampleRate);
	}
	if (HDA_PARAM_SUPP_PCM_SIZE_RATE_32BIT(supPcmSizeRates)) {
		format.fBitDepth = 32;
		format.fBitWidth = 32;
            formatEx.fBytesPerPacket = format.fNumChannels * (format.fBitWidth / 8);
            mStream->addAvailableFormat(&format, &formatEx, &sampleRate, &sampleRate);
        }
	}

	if (!format.fBitDepth || !format.fBitWidth) {
		errorMsg("error: couldn't find supported bit depth (16, 24, or 32-bit)\n");
		goto done;
    }

	sampleRate.whole = defaultSampleRate;
	setSampleRate(&sampleRate);

	addAudioStream(mStream);

	mStream->setFormat(&format, &formatEx); // set widest format as default

	result = true;
done:
	RELEASE(mStream);

	return result;
}

IOAudioStreamDirection VoodooHDAEngine::getEngineDirection()
{
	IOAudioStreamDirection direction;

	if (mChannel->direction == PCMDIR_PLAY) {
		ASSERT(mChannel->pcmDevice->playChanId >= 0);
		direction = kIOAudioStreamDirectionOutput;
	} else if (mChannel->direction == PCMDIR_REC) {
		ASSERT(mChannel->pcmDevice->recChanId >= 0);
		direction = kIOAudioStreamDirectionInput;
	} else {
		BUG("invalid direction");
	}

	if (mStream)
		ASSERT(mStream->getDirection() == direction);

	return direction;
}

int VoodooHDAEngine::getEngineId()
{
	if (getEngineDirection() == kIOAudioStreamDirectionOutput)
		return mChannel->pcmDevice->playChanId;
	else
		return mChannel->pcmDevice->recChanId;
}

IOReturn VoodooHDAEngine::performAudioEngineStart()
{
//	logMsg("VoodooHDAEngine[%p]::performAudioEngineStart\n", this);

//	logMsg("calling channelStart() for channel %d\n", getEngineId());
	takeTimeStamp(false);
	mDevice->channelStart(mChannel);

	return kIOReturnSuccess;
}

IOReturn VoodooHDAEngine::performAudioEngineStop()
{
//	logMsg("VoodooHDAEngine[%p]::performAudioEngineStop\n", this);

//	logMsg("calling channelStop() for channel %d\n", getEngineId());
	mDevice->channelStop(mChannel);

	return kIOReturnSuccess;
}

UInt32 VoodooHDAEngine::getCurrentSampleFrame()
{
	return (mDevice->channelGetPosition(mChannel) / mSampleSize);
}

// pauseAudioEngine, beginConfigurationChange, completeConfigurationChange, resumeAudioEngine

IOReturn VoodooHDAEngine::performFormatChange(IOAudioStream *audioStream,
											  const IOAudioStreamFormat *newFormat,
											  const IOAudioSampleRate *newSampleRate)
{
	IOReturn result = kIOReturnError;
	int setResult;
	UInt32 ossFormat;

	// ASSERT(audioStream == mStream);

//	logMsg("VoodooHDAEngine[%p]::peformFormatChange(%p, %p, %p)\n", this, audioStream, newFormat,
//			newSampleRate);

	if (!newSampleRate)
		newSampleRate = getSampleRate();
	if (!newFormat && !newSampleRate) {
		errorMsg("warning: performFormatChange(%p) called with no effect\n", audioStream);
		return kIOReturnSuccess;
	}

	if (newFormat) {
	int channels = newFormat->fNumChannels;

        if(!channels) {
            channels = 2;
        }

			ossFormat = AFMT_STEREO;

        if (newFormat->fSampleFormat == kIOAudioStreamSampleFormat1937AC3) {
            ossFormat = AFMT_AC3;
		} else if (channels == 4) {
			ossFormat = SND_FORMAT(0, 4, 0);
		} else if (channels == 6) {
			ossFormat = SND_FORMAT(0, 6, 1);
		} else if (channels == 8) {
			ossFormat = SND_FORMAT(0, 8, 1);
		}

		ASSERT(newFormat->fNumericRepresentation == kIOAudioStreamNumericRepresentationSignedInt);
		ASSERT(newFormat->fAlignment == kIOAudioStreamAlignmentLowByte);
		ASSERT(newFormat->fByteOrder == kIOAudioStreamByteOrderLittleEndian);

        if(ossFormat != AFMT_AC3) {
		switch (newFormat->fBitDepth) {
			case 16:
				ASSERT(newFormat->fBitWidth == 16);
				ossFormat |= AFMT_S16_LE;
				break;
			case 20:
            case 24:
			case 32:
				ASSERT(newFormat->fBitWidth == 32);
				ossFormat |= AFMT_S32_LE;
				break;
			default:
				BUG("unsupported bit depth");
				goto done;
		}
        }
		//IOLog("ossFormat=%08x\n", (unsigned int)ossFormat);

		setResult = mDevice->channelSetFormat(mChannel, ossFormat);
	//	logMsg("channelSetFormat(0x%08lx) for channel %d returned %d\n", ossFormat, getEngineId(),
	//			setResult);
		if (setResult != 0) {
			errorMsg("error: couldn't set format 0x%lx (%d-bit depth)\n", (long unsigned int)ossFormat, newFormat->fBitDepth);
			goto done;
		}

		ASSERT(mBufferSize);
/*		if (channels == 0) {
			channels = 2;
		} */
		mSampleSize = channels * (newFormat->fBitWidth / 8);
		mNumSampleFrames = mBufferSize / mSampleSize;
		setNumSampleFramesPerBuffer(mNumSampleFrames);

		logMsg("buffer size: %ld, channels: %d, bit depth: %d, # samp. frames: %ld\n", (long int)mBufferSize,
				channels, newFormat->fBitDepth, (long int)mNumSampleFrames);
		//goto done;
	}

	if (newSampleRate) {
		setResult = mDevice->channelSetSpeed(mChannel, newSampleRate->whole);
//		logMsg("channelSetSpeed(%ld) for channel %d returned %d\n", newSampleRate->whole, getEngineId(),
//				setResult);
		if ((UInt32) setResult != newSampleRate->whole) {
			errorMsg("error: couldn't set sample rate %ld\n", (long int)newSampleRate->whole);
			goto done;
		}
	}

	result = kIOReturnSuccess;
done:
	return result;
}

static
IOReturn SelectorChanged(OSObject*, IOAudioControl*, SInt32, SInt32)
{
	return kIOReturnSuccess;
}

bool VoodooHDAEngine::createAudioControls()
{
	bool			result = false;
	IOAudioControl	*control;
	IOAudioStreamDirection direction;
	UInt32			usage;
	UInt64			minMaxDb;
	IOFixed			minDb,
					maxDb;
	int				initOssDev, initOssMask, idupper;

	direction = getEngineDirection();
	if (direction == kIOAudioStreamDirectionOutput) {
		usage = kIOAudioControlUsageOutput;
		initOssDev = SOUND_MIXER_VOLUME;
		initOssMask = SOUND_MASK_VOLUME;
	}
	else if (direction == kIOAudioStreamDirectionInput) {
		usage = kIOAudioControlUsageInput;
		initOssDev = SOUND_MIXER_MIC;
		initOssMask = SOUND_MASK_MIC | SOUND_MASK_MONITOR;
	}
	else {
		errorMsg("uknown direction\n");
		goto Done;
	}

	idupper = mChannel->streamId << 16;

	if (mChannel->funcGroup->audio.assocs[mChannel->assocNum].digital)
		goto createSelectorControl;

	minMaxDb = getMinMaxDb(initOssMask);
	minDb = (IOFixed) (minMaxDb >> 32);
	maxDb = (IOFixed) (minMaxDb & ~0UL);
//	logMsg("minDb: %d (%08lx), maxDb: %d (%08lx)\n", (SInt16) (minDb >> 16), minDb,
//		   (SInt16) (maxDb >> 16), maxDb);
	if ((minDb == ~0L) || (maxDb == ~0L)) {
		//logMsg("warning: found invalid min/max dB (using default -22.5 -> 0.0dB range)\n"); //-22.5 -> 0.0
		minDb = (-22 << 16) + (65536 / 2);
		maxDb = 0 << 16;
	}

	/* Create Volume controls */
	/* Left channel */
	control = IOAudioLevelControl::createVolumeControl(mDevice->mMixerDefaults[initOssDev],
													   0,
													   100,
													   minDb,
													   maxDb,
													   kIOAudioControlChannelIDDefaultLeft,
													   kIOAudioControlChannelNameLeft,
													   idupper | 0U,
													   usage);
    if (!control) {
		errorMsg("error: createVolumeControl failed\n");
        goto Done;
    }

    control->setValueChangeHandler((IOAudioControl::IntValueChangeHandler)volumeChangeHandler, this);
    this->addDefaultAudioControl(control);
    control->release();

	/* Right channel */
	control = IOAudioLevelControl::createVolumeControl(mDevice->mMixerDefaults[initOssDev],
													   0,
													   100,
													   minDb,
													   maxDb,
													   kIOAudioControlChannelIDDefaultRight,
													   kIOAudioControlChannelNameRight,
													   idupper | 1U,
													   usage);
    if (!control) {
		errorMsg("error: createVolumeControl failed\n");
        goto Done;
    }

    control->setValueChangeHandler((IOAudioControl::IntValueChangeHandler)volumeChangeHandler, this);
    this->addDefaultAudioControl(control);
    control->release();

	// Create mute control
    control = IOAudioToggleControl::createMuteControl(false,	// initial state - unmuted
													  kIOAudioControlChannelIDAll,	// Affects all channels
													  kIOAudioControlChannelNameAll,
													  idupper | 2U,
													  usage);
    if (!control) {
		errorMsg("error: createMuteControl failed\n");
        goto Done;
    }

    control->setValueChangeHandler((IOAudioControl::IntValueChangeHandler)muteChangeHandler, this);
    this->addDefaultAudioControl(control);
    control->release();

#if 0
    // Create a left & right input gain control with an int range from 0 to 65535
    // and a db range from 0 to 22.5
    control = IOAudioLevelControl::createVolumeControl(80,	// Initial value
													   0,		// min value
													   100,	// max value
													   0,		// min 0.0 in IOFixed
													   (22 << 16) + (32768),	// 22.5 in IOFixed (16.16)
													   kIOAudioControlChannelIDDefaultLeft,
													   kIOAudioControlChannelNameLeft,
													   idupper | 3U,		// control ID - driver-defined
													   kIOAudioControlUsageInput);
    if (!control) {
        goto Done;
    }

    control->setValueChangeHandler((IOAudioControl::IntValueChangeHandler)gainChangeHandler, this);
    this->addDefaultAudioControl(control);
    control->release();

    control = IOAudioLevelControl::createVolumeControl(80,	// Initial value
													   0,		// min value
													   100,	// max value
													   0,		// min 0.0 in IOFixed
													   (22 << 16) + (32768),	// max 22.5 in IOFixed (16.16)
													   kIOAudioControlChannelIDDefaultRight,	// Affects right channel
													   kIOAudioControlChannelNameRight,
													   idupper | 4U,		// control ID - driver-defined
													   kIOAudioControlUsageInput);
    if (!control) {
        goto Done;
    }

    control->setValueChangeHandler((IOAudioControl::IntValueChangeHandler)gainChangeHandler, this);
    this->addDefaultAudioControl(control);
    control->release();
#endif

createSelectorControl:
	if(usage == kIOAudioControlUsageOutput) {
		mSelControl = IOAudioSelectorControl::createOutputSelector(mPortType,
																   kIOAudioControlChannelIDAll,
																   kIOAudioControlChannelNameAll,
																   idupper | 5U);
	}else{
		mSelControl = IOAudioSelectorControl::createInputSelector(mPortType,
																  kIOAudioControlChannelIDAll,
																  kIOAudioControlChannelNameAll,
																  idupper | 5U);
	}
	if(mSelControl != 0) {
		mSelControl->addAvailableSelection(mPortType, mPortName);
		mSelControl->setValueChangeHandler(SelectorChanged, this);
		this->addDefaultAudioControl(mSelControl);
		//enumiratePinNames();
	}

	result = true;

Done:
	return result;
}

void VoodooHDAEngine::setPinName(UInt32 pinConfig, const char* name)
{
	UInt32 previousPortType;
	if (!name)
		return;
	previousPortType = mPortType;
	mPortName = name;
	mPortType = pinConfigToSelection(pinConfig);
	beginConfigurationChange();
	setDescription(name);
	if(mSelControl == 0) {
		completeConfigurationChange();
		return;
	}

	mSelControl->removeAvailableSelection(previousPortType);
	mSelControl->addAvailableSelection(mPortType, name);
	mSelControl->setValue(mPortType);
	completeConfigurationChange();
}

__attribute__((visibility("hidden")))
IOReturn VoodooHDAEngine::volumeChangeHandler(IOService *target, IOAudioControl *volumeControl, SInt32 oldValue, SInt32 newValue)
{
	IOReturn result = kIOReturnBadArgument;
	VoodooHDAEngine *audioEngine = OSDynamicCast(VoodooHDAEngine, target);

//	audioEngine = (VoodooHDAEngine *)target;
	if (audioEngine) {
		result = audioEngine->volumeChanged(volumeControl, oldValue, newValue);
	}

	return result;
}

IOReturn VoodooHDAEngine::volumeChanged(IOAudioControl *volumeControl, SInt32 oldValue, SInt32 newValue)
{
	if(mVerbose >2)
		errorMsg("VoodooHDAEngine[%p]::volumeChanged(%p, %ld, %ld)\n", this, volumeControl, (long int)oldValue, (long int)newValue);

	if (volumeControl) {

		int ossDev = ( getEngineDirection() == kIOAudioStreamDirectionOutput) ? SOUND_MIXER_VOLUME:
		SOUND_MIXER_MIC;

		PcmDevice *pcmDevice = mChannel->pcmDevice;

		switch (ossDev) {
			case SOUND_MIXER_VOLUME:
				/* Left channel */
				if(volumeControl->getChannelID() == 1) {
					oldOutVolumeLeft = newValue;
					if (mEnableVolumeChangeFix) {
						mDevice->audioCtlOssMixerSet(pcmDevice, SOUND_MIXER_PCM, newValue, newValue);
					} else {
						mDevice->audioCtlOssMixerSet(pcmDevice, SOUND_MIXER_VOLUME, newValue, pcmDevice->right[0]);
					}

				}
				/* Right channel */
				else if(volumeControl->getChannelID() == 2) {
					oldOutVolumeRight = newValue;
					if (mEnableVolumeChangeFix) {
						mDevice->audioCtlOssMixerSet(pcmDevice, SOUND_MIXER_PCM, newValue, newValue);
					} else {
						mDevice->audioCtlOssMixerSet(pcmDevice, SOUND_MIXER_VOLUME, pcmDevice->left[0], newValue);
					}
				}

				break;
			case SOUND_MIXER_MIC:
				oldInputGain = newValue;
				mDevice->audioCtlOssMixerSet(pcmDevice, ossDev, newValue, newValue);
				break;
			default:
				break;
		}
		// cue8chalk: this seems to be needed when pin configs aren't set properly
		if (mEnableVolumeChangeFix) {
			for (int n = 0; n < SOUND_MIXER_NRDEVICES; n++){
				mDevice->audioCtlOssMixerSet(pcmDevice, n, newValue, newValue);
			}
		}

	}

	return kIOReturnSuccess;
}

__attribute__((visibility("hidden")))
IOReturn VoodooHDAEngine::muteChangeHandler(IOService *target, IOAudioControl *muteControl, SInt32 oldValue, SInt32 newValue)
{
	IOReturn result = kIOReturnBadArgument;
	VoodooHDAEngine *audioEngine = OSDynamicCast(VoodooHDAEngine, target);

//	audioEngine = (VoodooHDAEngine *)target;
	if (audioEngine) {
		result = audioEngine->muteChanged(muteControl, oldValue, newValue);
	}

	return result;
}

IOReturn VoodooHDAEngine::muteChanged(IOAudioControl *muteControl, SInt32 oldValue, SInt32 newValue)
{
	if(mVerbose >2)
		errorMsg("VoodooHDAEngine[%p]::outputMuteChanged(%p, %ld, %ld)\n", this, muteControl, (long int)oldValue, (long int)newValue);

	int ossDev = ( getEngineDirection() == kIOAudioStreamDirectionOutput) ? SOUND_MIXER_VOLUME:
																			SOUND_MIXER_MIC;

	PcmDevice *pcmDevice = mChannel->pcmDevice;

	if (newValue) {
        // VertexBZ: Mute fix
        if(mEnableMuteFix){
          mDevice->audioCtlOssMixerSet(pcmDevice, SOUND_MIXER_PCM, 0, 0);
        } else {
          mDevice->audioCtlOssMixerSet(pcmDevice, ossDev, 0, 0);
        }
	} else {
        // VertexBZ: Mute fix
        if(mEnableMuteFix){
            mDevice->audioCtlOssMixerSet(pcmDevice, SOUND_MIXER_PCM,
                                         (ossDev == SOUND_MIXER_VOLUME) ? oldOutVolumeLeft : oldInputGain,
                                         (ossDev == SOUND_MIXER_VOLUME) ? oldOutVolumeRight: oldInputGain);
        } else {

          mDevice->audioCtlOssMixerSet(pcmDevice, ossDev,
									   (ossDev == SOUND_MIXER_VOLUME) ? oldOutVolumeLeft : oldInputGain,
									   (ossDev == SOUND_MIXER_VOLUME) ? oldOutVolumeRight: oldInputGain);
		}
	}

    return kIOReturnSuccess;
}

__attribute__((visibility("hidden")))
IOReturn VoodooHDAEngine::gainChangeHandler(IOService *target, IOAudioControl *gainControl, SInt32 oldValue, SInt32 newValue)
{
	IOReturn result = kIOReturnBadArgument;
	VoodooHDAEngine *audioDevice = OSDynamicCast(VoodooHDAEngine, target);

//	audioDevice = (VoodooHDAEngine *)target;
	if (audioDevice) {
		result = audioDevice->gainChanged(gainControl, oldValue, newValue);
	}

	return result;
}

IOReturn VoodooHDAEngine::gainChanged(IOAudioControl *gainControl, SInt32 oldValue, SInt32 newValue)
{
    IOLog("VoodooHDAEngine[%p]::gainChanged(%p, %ld, %ld)\n", this, gainControl, (long int)oldValue, (long int)newValue);

    if (gainControl) {
       IOLog("\t-> Channel %ld\n", (long int)gainControl->getChannelID());
    }

    // Add hardware gain change code here

    return kIOReturnSuccess;
}

OSString *VoodooHDAEngine::getLocalUniqueID()
{
	if (!mDevice || !mDevice->mPciNub)
			return super::getLocalUniqueID();

	OSString *ioName = OSDynamicCast(OSString, mDevice->mPciNub->getProperty("IOName"));
	if (!ioName)
			return super::getLocalUniqueID();

	char str[64] = "";
	snprintf(str, sizeof str, "%s:%lx", ioName->getCStringNoCopy(), (long unsigned int)index);
	return OSString::withCString(str);
}
