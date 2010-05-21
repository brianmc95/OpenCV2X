#include "SNRThresholdDeciderNew.h"

simtime_t SNRThresholdDeciderNew::processNewSignal(AirFrame* frame)
{
	//the rssi level changes therefore we need to check if we can
	//answer an ongoing ChannelSenseRequest now
	channelStateChanged();

	if(currentSignal.first != 0) {
		debugEV << "Already receiving another AirFrame!" << endl;
		return notAgain;
	}

	// get the receiving power of the Signal at start-time
	Signal& signal = frame->getSignal();
	double recvPower = signal.getReceivingPower()->getValue(Argument(signal.getSignalStart()));

	// check whether signal is strong enough to receive
	if ( recvPower < sensitivity )
	{
		debugEV << "Signal is to weak (" << recvPower << " < " << sensitivity
				<< ") -> do not receive." << endl;
		// Signal too weak, we can't receive it, tell PhyLayer that we don't want it again
		return notAgain;
	}

	// Signal is strong enough, receive this Signal and schedule it
	debugEV << "Signal is strong enough (" << recvPower << " > " << sensitivity
			<< ") -> Trying to receive AirFrame." << endl;

	currentSignal.first = frame;
	currentSignal.second = EXPECT_END;

	return ( signal.getSignalStart() + signal.getSignalLength() );
}

// TODO: for now we check a larger mapping within an interval
bool SNRThresholdDeciderNew::checkIfAboveThreshold(Mapping* map, simtime_t start, simtime_t end)
{
	assert(map);

	if(debug){
		debugEV << "Checking if SNR is above Threshold of " << snrThreshold << endl;
	}

	// check every entry in the mapping against threshold value
	ConstMappingIterator* it = map->createConstIterator(Argument(start));
	// check if values at start-time fulfill snrThreshold-criterion
	if(debug){
		debugEV << "SNR at time " << start << " is " << it->getValue() << endl;
	}
	if ( it->getValue() <= snrThreshold ){
		delete it;
		return false;
	}

	while ( it->hasNext() && it->getNextPosition().getTime() < end)
	{
		it->next();

		if(debug){
			debugEV << "SNR at time " << it->getPosition().getTime() << " is " << it->getValue() << endl;
		}

		// perform the check for smaller entry
		if ( it->getValue() <= snrThreshold) {
			delete it;
			return false;
		}
	}

	it->iterateTo(Argument(end));
	if(debug){
		debugEV << "SNR at time " << end << " is " << it->getValue() << endl;
	}

	if ( it->getValue() <= snrThreshold ){
		delete it;
		return false;
	}

	delete it;
	return true;
}

ChannelState SNRThresholdDeciderNew::getChannelState() {

	simtime_t now = phy->getSimTime();
	double rssiValue = calcChannelSenseRSSI(now, now);

	return ChannelState(isIdleRSSI(rssiValue), rssiValue);
}

void SNRThresholdDeciderNew::answerCSR(CSRInfo& requestInfo)
{
	// put the sensing-result to the request and
	// send it to the Mac-Layer as Control-message (via Interface)
	requestInfo.first->setResult( getChannelState() );
	phy->sendControlMsg(requestInfo.first);

	requestInfo.first = 0;
	requestInfo.second = -1;
	requestInfo.canAnswerAt = -1;
}

simtime_t SNRThresholdDeciderNew::canAnswerCSR(const CSRInfo& requestInfo) {
	const ChannelSenseRequest* request = requestInfo.getRequest();
	assert(request);

	simtime_t requestTimeout = requestInfo.getSenseStart() + request->getSenseTimeout();

	if(request->getSenseMode() == UNTIL_TIMEOUT) {
		return requestTimeout;
	}

	simtime_t now = phy->getSimTime();

	ConstMapping* rssiMapping = calculateRSSIMapping(now, requestTimeout);

	//this Decider only works for time-only signals
	assert(rssiMapping->getDimensionSet() == DimensionSet::timeDomain);

	ConstMappingIterator* it = rssiMapping->createConstIterator(Argument(now));

	assert(request->getSenseMode() == UNTIL_IDLE
		   || request->getSenseMode() == UNTIL_BUSY);
	bool untilIdle = request->getSenseMode() == UNTIL_IDLE;

	simtime_t answerTime = requestTimeout;
	//check if the current rssi value enables us to answer the request
	if(isIdleRSSI(it->getValue()) == untilIdle) {
		answerTime = now;
	}
	else {
		//iterate through request interval to check when the rssi level
		//changes such that the request can be answered
		while(it->hasNext() && it->getNextPosition().getTime() < requestTimeout) {
			it->next();

			if(isIdleRSSI(it->getValue()) == untilIdle) {
				answerTime = it->getPosition().getTime();
				break;
			}
		}
	}

	delete it;
	delete rssiMapping;

	return answerTime;
}

simtime_t SNRThresholdDeciderNew::processSignalEnd(AirFrame* frame)
{
	assert(frame == currentSignal.first);
	// here the Signal is finally processed

	// first collect all necessary information
	Mapping* snrMap = calculateSnrMapping(frame);
	assert(snrMap);

	//TODO: this extraction is just temporary, since we need to pass Signal's start-
	// and end-time to the following method
	const Signal& signal = frame->getSignal();
	simtime_t start = signal.getSignalStart();
	simtime_t end = start + signal.getSignalLength();

	bool aboveThreshold = checkIfAboveThreshold(snrMap, start, end);

	// check if the snrMapping is above the Decider's specific threshold,
	// i.e. the Decider has received it correctly
	if (aboveThreshold)
	{
		debugEV << "SNR is above threshold("<<snrThreshold<<") -> sending up." << endl;
		// go on with processing this AirFrame, send it to the Mac-Layer
		phy->sendUp(frame, new DeciderResult(true));
	} else
	{
		debugEV << "SNR is below threshold("<<snrThreshold<<") -> dropped." << endl;
	}

	delete snrMap;
	snrMap = 0;


	// we have processed this AirFrame and we prepare to receive the next one
	currentSignal.first = 0;

	return notAgain;
}
