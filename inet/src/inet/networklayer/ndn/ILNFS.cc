//
// Copyright (c) 2018 Amar Abane (a_abane@hotmail.fr). All rights reserved.
// This file is part of NDN-Omnet.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with this program.  If not, see http://www.gnu.org/licenses/.
//

#include "ILNFS.h"

namespace inet {
using std::cout;

Define_Module(ILNFS);

enum NdnMacMappingCodes {
    IB_DB = 11, // Interest broadcast, Data broadcast
    IB_DU,      // Interest broadcast, Data unicast
    IU_DB,      // Interest unicast, Data broadcast
    IU_DU,      // Interest unicast, Data unicast
};

ILNFS::ILNFS()
{
}

ILNFS::~ILNFS()
{
    if (waiting)
        delete sendDelayedPacket->getDelayedPacket();
    cancelAndDelete(sendDelayedPacket);
}

void ILNFS::initialize(int stage)
    {
    cSimpleModule::initialize(stage);

    if (stage == INITSTAGE_LOCAL) {
        pit = getModuleFromPar<PitBase>(par("pitModule"), this);
        fib = getModuleFromPar<FibIlnfs>(par("fibModule"), this);
        cs = getModuleFromPar<CsBase>(par("csModule"), this);

        cModule *pitModule = check_and_cast<cModule *>(pit);
        pitModule->subscribe(PitBase::interestTimeoutSignal, this);

        cModule *fibModule = check_and_cast<cModule *>(fib);
        fibModule->subscribe(FibIlnfs::entryExpiredSignal, this);

        cModule *csModule = check_and_cast<cModule *>(fib);
        csModule->subscribe(CsBase::dataStaleSignal, this);

        ift = getModuleFromPar<IInterfaceTable>(par("interfaceTableModule"), this);

        ndnMacMapping = par("ndnMacMapping");
        forwarding = par("forwarding");
        cacheUnsolicited = par("cacheUnsolicited");

        WATCH(numIntReceived);
        WATCH(numIntFwd);
        WATCH(numIntCanceled);
        WATCH(numDataCanceled);
        WATCH(numDataReceived);
        WATCH(numDataUnsolicited);
        WATCH(numDataFwd);

    }else if(stage == INITSTAGE_NETWORK_LAYER){
        for (int i = 0; i < ift->getNumInterfaces(); i++)
        {
            myMacAddress = ift->getInterface(i)->getMacAddress();
            if ( myMacAddress.equals(MACAddress("00-00-00-00-00-01")) )
                break;
        }
        cout << getFullPath() << ": MAC address: " << myMacAddress.str().c_str() << endl;
    }
}

void ILNFS::handleMessage(cMessage *msg)
{
    if ( msg == sendDelayedPacket ){
        SendDelayed *sd  = check_and_cast<SendDelayed *>(msg);
        if( sd->getType() == INTEREST ){
            Interest * interest = check_and_cast<Interest *>(sd->getDelayedPacket());
            forwardInterestToRemote(interest->dup(), sd->getFace(), MACAddress(sd->getMacSrc()), MACAddress(sd->getMacDest()));
            numIntFwd++;
        }
        else{
            Data *data = check_and_cast<Data *>(sd->getDelayedPacket());
            forwardDataToRemote(data->dup(), sd->getFace(), MACAddress(sd->getMacDest()));
            numDataFwd++;
        }
        delete sd->getDelayedPacket();
        waiting = false;
    }
    else if ( msg->isPacket() ){
        if ( msg->getKind() == REGISTER_PREFIX_CODE){
            cout << simTime() << "\t" << getFullPath() << ": Register prefix (" << msg->getName() << ")" << endl;
            fib->registerPrefix(msg->getName(), msg->getArrivalGate(), myMacAddress);
            delete msg;
            return;
        }
        NdnPacket* ndnPacket = check_and_cast<NdnPacket *>(msg);
        if ( waiting ){
            if ( !strcmp(ndnPacket->getName(), sendDelayedPacket->getDelayedPacket()->getName()) ){
                cout << simTime() << "\t" << getFullPath() << ": Cancel delayed transmission ("
                        << sendDelayedPacket->getDelayedPacket()->getName() << "), Type: "
                        << sendDelayedPacket->getDelayedPacket()->getType() << endl;
                cancelEvent(sendDelayedPacket);
                delete sendDelayedPacket->getDelayedPacket();
                waiting = false;
                if (ndnPacket->getType() == INTEREST)
                    numIntCanceled++;
                else
                    numDataCanceled++;
            }
            delete ndnPacket;
            return;
        }
        else
            dispatchPacket(check_and_cast<NdnPacket *>(msg));
    }else
        delete msg;
}

void ILNFS::dispatchPacket(NdnPacket *packet)
{
    if( packet->getArrivalGate()->isName("lowerLayerIn") ){
        //Ieee802Ctrl *ctrl = check_and_cast<Ieee802Ctrl *>(packet->getControlInfo());
        IMACProtocolControlInfo *ctrl = check_and_cast<IMACProtocolControlInfo *>(packet->getControlInfo());
        packet->removeControlInfo();
        if ( packet->getType() == INTEREST ){
            processLLInterest(check_and_cast<Interest *>(packet), ctrl->getSourceAddress());
        }
        else if ( packet->getType() == DATA ){
            processLLData(check_and_cast<Data *>(packet), ctrl->getSourceAddress());
        }
    }
    else if( packet->getArrivalGate()->isName("upperLayerIn") ){
        if ( packet->getType() == INTEREST )
            processHLInterest(check_and_cast<Interest *>(packet));
        else if ( packet->getType() == DATA )
            processHLData(check_and_cast<Data *>(packet));
    }
    else{
        cout << simTime() << "\t" << getFullPath() << ": << Unexpected packet origin (" << packet->getName() << ")" << endl;
        delete packet;
    }
}

void ILNFS::processLLInterest(Interest *interest, MACAddress macSrc)
{
    numIntReceived++;
    cout << simTime() << "\t" << getFullPath() << ": << Interest from LL (" << interest->getName() << ")" << endl;
    Data* cachedData = cs->lookup(interest);
    if ( cachedData != nullptr ){
        cout << simTime() << "\t" << getFullPath() << ": << Cached Data found (" << interest->getName() << ")" << endl;
        forwardDataToRemote(cachedData->dup(), interest->getArrivalGate()->getIndex(), macSrc);
        delete interest;
        return;
    }
    if( pit->lookup(interest->getName()) == nullptr ){
        neighborI++;
        cout << simTime() << "\t" << getFullPath() << ": New Interest" << endl;
        IlnfsEntry *fe = (IlnfsEntry*) fib->lookup(interest);
        if ( fe != nullptr ){
            if ( fe->getFace()->isName("lowerLayerIn") && forwarding ){
                /* iLNFS */
                float myCost = fe->getCost();
                float delta = 0;
                if( interest->getCost() == 0. )
                    delta = (float) (DELTA_MAX - myCost);
                else                            // other cases
                    delta = (float) (interest->getCost() - myCost);
                if( delta < 0 ){                 // eligibility test
                    delete interest;
                    return;
                }
                delta+=computeTheta();          // delay adjustment
                interest->setHopCount(interest->getHopCount() + 1);
                interest->setCost(myCost);

                /*float prob = 0.25;
                if (delta >= 0 && delta < DELTA_MAX)
                    prob = 0.8 / (DELTA_MAX - delta);
                if (prob > 1)
                    prob = 1;
                interest->setPriority(prob);*/

                if( waiting )
                    delete sendDelayedPacket->getDelayedPacket();
                sendDelayedPacket->setDelayedPacket(interest);
                sendDelayedPacket->setType(INTEREST);
                sendDelayedPacket->setFace(fe->getFace()->getIndex());
                sendDelayedPacket->setMacDest(fe->getMacDest().str().c_str());
                sendDelayedPacket->setMacSrc(macSrc.str().c_str());
                scheduleAt(simTime() + SimTime(computeDelay(delta),SIMTIME_MS), sendDelayedPacket);
                waiting = true;

                //forwardInterestToRemote(interest, fe->getFace()->getIndex(), macSrc, fe->getMacDest());
                //numIntFwd++;
            }
            else if ( fe->getFace()->isName("upperLayerIn") ){
                interest->setHopCount(interest->getHopCount() + 1);
                forwardInterestToLocal(interest, fe->getFace()->getIndex(), macSrc);
            }
            else
                delete interest;
        }
        else{       // unknow name (myCost = 0)
            if( interest->getCost() == 0. ){
                interest->setHopCount(interest->getHopCount() + 1);
                interest->setPriority(1);
                if( waiting )
                    delete sendDelayedPacket->getDelayedPacket();
                sendDelayedPacket->setDelayedPacket(interest);
                sendDelayedPacket->setType(INTEREST);
                sendDelayedPacket->setFace(DEFAULT_MAC_IF);
                sendDelayedPacket->setMacDest("ff:ff:ff:ff:ff:ff");
                sendDelayedPacket->setMacSrc(macSrc.str().c_str());
                scheduleAt(simTime() + SimTime(computeInterestRandomDelay(),SIMTIME_MS), sendDelayedPacket);
                waiting = true;
            }
            else{
                delete interest;
                return;
            }
        }
    }
    else{
        cout << simTime() << "\t" << getFullPath() << ": Interest already in PIT" << endl;
        numIntDuplicated++;
        delete interest;
    }
}

void ILNFS::processLLData(Data *data, MACAddress macSrc)
{
    cout << simTime() << "\t" << getFullPath() << ": << Data from LL (" << data->getName() << ")" << endl;
    neighborD++;
    IPit::PitEntry *pe = pit->lookup(data->getName());
    if ( pe != nullptr ){
        cout << simTime() << "\t" << getFullPath() << ": Solicited Data" << endl;
        numDataReceived++;
        float newCost = fib->updateCost(data->getName(), data->getPrefixLength(), data->getArrivalGate(), macSrc, data->getCost(), ALPHA);
        pit->remove(data->getName());
        if ( pe->getFace()->isName("lowerLayerIn") && forwarding ){
            cout << simTime() << "\t" << getFullPath() << ": Delay forward Data through NetDeviceFace" << endl;
            data->setHopCount(data->getHopCount()+1);
            data->setCost(newCost);
            forwardDataToRemote(data, pe->getFace()->getIndex(), pe->getMacSrc());
            numDataFwd++;
            cs->add(data);
        }
        else if ( pe->getFace()->isName("upperLayerIn") ){
            data->setHopCount(data->getHopCount()+1);
            forwardDataToLocal(data, pe->getFace()->getIndex());
            cs->add(data);
        }
        else
            delete data;
    }
    else{
        cout << simTime() << "\t" << getFullPath() << ": Unsolicited Data" << endl;
        fib->updateCost(data->getName(), data->getPrefixLength(), data->getArrivalGate(), macSrc, data->getCost(), ALPHA);
        if (cacheUnsolicited)
            cs->add(data);
        delete data;
        numDataUnsolicited++;
    }
}

void ILNFS::processHLInterest(Interest *interest)
{
    cout << simTime() << "\t" << getFullPath() << ": << HL Interest (" << interest->getName() << ")" << endl;
    Data* cachedData = cs->lookup(interest);
    if ( cachedData != nullptr ){
        cout << simTime() << "\t" << getFullPath() << ": << Cached Data found (" << interest->getName() << ")" << endl;
        forwardDataToLocal(cachedData->dup(), interest->getArrivalGate()->getIndex());
        delete interest;
        return;
    }
    if( pit->lookup(interest->getName()) == nullptr ){
        cout << simTime() << "\t" << getFullPath() << ": New Interest" << endl;
        IlnfsEntry *fe = (IlnfsEntry*) fib->lookup(interest);
        if ( fe != nullptr ){
            if ( fe->getFace()->isName("lowerLayerIn") ){
                interest->setCost(fe->getCost());
                interest->setPriority(1);
                forwardInterestToRemote(interest, fe->getFace()->getIndex(), myMacAddress, fe->getMacDest());
            }
            else if ( fe->getFace()->isName("upperLayerIn") ){
                forwardInterestToLocal(interest, fe->getFace()->getIndex(), myMacAddress);
            }
        }
        else{
            interest->setCost(0);
            interest->setPriority(1);
            forwardInterestToRemote(interest, DEFAULT_MAC_IF, myMacAddress, MACAddress("ff:ff:ff:ff:ff:ff"));
        }
    }
    else{
        cout << simTime() << "\t" << getFullPath() << ": Interest already in PIT" << endl;
        numIntDuplicated++;
        delete interest;
    }
}

void ILNFS::processHLData(Data *data)
{
    IPit::PitEntry *pe = pit->lookup(data->getName());
    cout << simTime() << "\t" << getFullPath() << ": << Data from UL (" << data->getName() << ")" << endl;
    if ( pe != nullptr ){
        cout << simTime() << "\t" << getFullPath() << ": Solicited Data from AppFace " << data->getArrivalGate()->getIndex() << endl;
        if ( pe->getFace()->isName("lowerLayerIn") ){
            data->setCost(0.);
            forwardDataToRemote(data, pe->getFace()->getIndex(), pe->getMacSrc());
        }
        else if ( pe->getFace()->isName("upperLayerIn") ){
            forwardDataToLocal(data, pe->getFace()->getIndex());
        }
        pit->remove(data->getName());
        cs->add(data);
    }
    else{
        cout << simTime() << "\t" << getFullPath() << ": Unsolicited Data" << endl;
        if (cacheUnsolicited)
            cs->add(data);
        delete data;
    }
}

void ILNFS::forwardInterestToRemote(Interest* interest, int face, MACAddress macSrc, MACAddress macDest)
{
    if ( pit->create(interest, macSrc) ){
        cout << simTime() << "\t" << getFullPath() << ": Send Interest through NetDeviceFace (" << interest->getName() << ")" << endl;
        ndnToMacMapping(interest, macDest);
        send(interest, "lowerLayerOut", face);
    }
}

void ILNFS::forwardDataToRemote(Data* data, int face, MACAddress macDest)
{
    cout << simTime() << "\t" << getFullPath() << ": Send Data through NetDeviceFace (" << data->getName() << ")" << endl;
    ndnToMacMapping(data, macDest);
    send(data, "lowerLayerOut", face);
}


void ILNFS::forwardInterestToLocal(Interest* interest, int face, MACAddress macSrc)
{
    if ( pit->create(interest, macSrc) ){
        cout << simTime() << "\t" << getFullPath() << ": Send Interest to AppFace (" << interest->getName() << ")" << endl;
        send(interest, "upperLayerOut", face);
    }
}

void ILNFS::forwardDataToLocal(Data* data, int face)
{
    cout << simTime() << "\t" << getFullPath() << ": Send Data to AppFace (" << data->getName() << ")" << endl;
    send(data, "upperLayerOut", face);
}


void ILNFS::ndnToMacMapping(NdnPacket *ndnPacket, MACAddress macDest)
{
    Ieee802Ctrl *controlInfo = new Ieee802Ctrl();
    //IMACProtocolControlInfo *controlInfo = new SimpleLinkLayerControlInfo();
    controlInfo->setSourceAddress(myMacAddress);

    if( ndnPacket->getType() == INTEREST ){
        if( ndnMacMapping == IB_DB || ndnMacMapping == IB_DU )
            controlInfo->setDestinationAddress(MACAddress("ff:ff:ff:ff:ff:ff"));
        else
            controlInfo->setDestinationAddress(macDest);
    }
    else if( ndnPacket->getType() == DATA ){
        if( ndnMacMapping == IB_DB || ndnMacMapping == IU_DB )
            controlInfo->setDestinationAddress(MACAddress("ff:ff:ff:ff:ff:ff"));
        else
            controlInfo->setDestinationAddress(macDest);
    }
    ndnPacket->setControlInfo(check_and_cast<cObject *>(controlInfo));
}

double ILNFS::computeInterestRandomDelay()
{
    return (DW + uniform(0,DW)) * DEFER_SLOT_TIME;
}

double ILNFS::computeDataRandomDelay()
{
    return uniform(0,DW-1) * DEFER_SLOT_TIME;
}

void ILNFS::refreshDisplay() const
{
    char buf[40];
    sprintf(buf, "rcvd: %d D\nfwd: %d I", numDataReceived, numIntFwd);
    getDisplayString().setTagArg("t", 0, buf);
}

void ILNFS::finish()
{
    recordScalar("numIntRcvd", numIntReceived);
    recordScalar("numIntFwd", numIntFwd);
    recordScalar("numIntCanceled", numIntCanceled);
    recordScalar("numIntDuplicated", numIntDuplicated);
    recordScalar("numDataReceived", numDataReceived);
    recordScalar("numDataFwd", numDataFwd);
    recordScalar("numDataCanceled", numDataCanceled);
    recordScalar("numDataUnsolicited", numDataUnsolicited);
}

void ILNFS::receiveSignal(cComponent *source, simsignal_t signalID, cObject *obj, cObject *details)
{
    Enter_Method_Silent();
    if (signalID == PitBase::interestTimeoutSignal) {
        IPit::Notification* notification = check_and_cast<IPit::Notification *>(obj);
        cGate* g = notification->face;
        if (g->isName("upperLayerIn")){
            Interest* interest = notification->interest->dup();
            interest->setKind(TIMEOUT_CODE);
            cout << simTime() << "\t" << getFullPath() << ": Send timeout. Face: " << g->getIndex() << " | Interest: " << interest->getName() << endl;
            send(interest, "upperLayerOut", g->getIndex());
        }
        fib->resetCost(notification->interest->getName(), DELTA_MAX);
    }
    else if (signalID == FibIlnfs::entryExpiredSignal) {
        IFib::Notification* notification = check_and_cast<IFib::Notification *>(obj);
        cout << simTime() << "\t" << getFullPath() << ": Expired prefix (" << notification->prefix << ")" << endl;
    }
}

bool ILNFS::handleOperationStage(LifecycleOperation *operation, int stage, IDoneCallback *doneCallback)
{
    Enter_Method_Silent();
    return true;
}

float ILNFS::computeTheta(){
    float Ns;
    int unsolicitedData = neighborD - numDataFwd;   // all received Data - fwd Data nbr
    int droppedI = neighborI - numIntFwd;           // all received Interest - fwd Interest nbr
    if (droppedI == 0){
        Ns = TH;
    }
    else{
        Ns = (double) unsolicitedData / (double) droppedI;
        if (Ns > 1) Ns = 1.;
    }
    return (float) (TH - Ns);
}

float ILNFS::computeDelay(float delta)
{
    return M / exp(delta*0.5) + N;
}

} // namespace inet

