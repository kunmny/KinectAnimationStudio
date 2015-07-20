#include "FBXCoding.h"


/// <summary>
/// Constructor
/// </summary>
/// <param name="enableInterleaving">Interleave packets before sending</param>
/// <param name="latency">Interleaving latency window</param>
FBXCoding::FBXCoding(bool enableInterleaving, int latency, bool enableLDPC, int ldpc_offset) :
p_isInterleavingMode(enableInterleaving),
p_latencyWindow(latency), 
p_enableLDPC(enableLDPC),
p_LDPC_offset(ldpc_offset),
p_fps(0)
{ 
	ConfigFileParser &parser = ConfigFileParser::getInstance();
	std::string latentsize = parser.getParameter(LATENCY_WINDOW);
	std::string interleave = parser.getParameter(ENABLE_INTERLEAVING);
	std::string ldpc = parser.getParameter(ENABLE_LDPC);
	std::string offset = parser.getParameter(LDPC_OFFSET);

	//std::string global = parser.getParameter(ENABLE_GLOBAL_TRANSFORMATION);

	if (latentsize.compare("ERROR") != 0) {
		p_latencyWindow = atoi(latentsize.c_str());
	}

	if (interleave.compare("ERROR") != 0) {
		std::istringstream(interleave) >> p_isInterleavingMode;
	}

	if (ldpc.compare("ERROR") != 0) {
		std::istringstream(ldpc) >> p_enableLDPC;

		if (p_enableLDPC) {
			char parityPath[512];
			GetLocalFile(c_defaultParityFileName, parityPath, 512);
			
			// Check if file exists
			if (INVALID_FILE_ATTRIBUTES == GetFileAttributes(parityPath) && GetLastError() == ERROR_FILE_NOT_FOUND) {
				// If it does not exist, let us generate parity
				// it++ construsts Parity Matrix
				H.generate(N_DATA_BIT + N_PARITY_BIT, 1, (N_DATA_BIT + N_PARITY_BIT)/N_PARITY_BIT, "rand", "200 6");
				H.save_alist(parityPath);
			}
			else {
				H.load_alist(parityPath);
			}

			// it++ constructs Generator Matrix;
			G.construct(&H);
			
		}
	}

	if (offset.compare("ERROR") != 0) {
		p_LDPC_offset = atoi(offset.c_str());
	}

}


/// <summary>
/// Destructor
/// </summary>
FBXCoding::~FBXCoding() { }


/// <summary>
/// Creates a map the relates node pointsers and their IDs
/// </summary>
void FBXCoding::initializeJointIdMap(FbxNode *parentNode) {

	int childCount = parentNode->GetChildCount();

	for (int i = 0; i < childCount; i++) {
		FbxNode* childI = parentNode->GetChild(i);
		p_jointMap[getCustomIdProperty(childI)] = childI;

		// Check if child has any translation curve
		if (childI->LclTranslation.IsAnimated())
			p_jointMap[TRANSLATION_CUSTOM_ID] = childI;
	}

}

void FBXCoding::encodeAnimation(FbxScene *lScene, FbxNode *markerSet, SOCKET s) {
	int Max_key_num;
	size_t fragmentSize;
	char *out_buf;

	if (p_enableLDPC) {
		// Maximum number of keys a package can store
		Max_key_num = PACKET_SIZE / sizeof(PACKET_LDPC);
		fragmentSize = sizeof(PACKET_LDPC);
		out_buf = (char *) new PACKET_LDPC[Max_key_num];
	}
	else {
		// Maximum number of keys a package can store
		Max_key_num = PACKET_SIZE / sizeof(PACKET);
		fragmentSize = sizeof(PACKET);
		out_buf = (char *) new PACKET[Max_key_num];
	}

	int pIndex = 0;

	// Vector used to store key indexes
	std::vector<int> keyIvec;
	keyIvec.reserve(p_latencyWindow);

	// We are ignoring the key at index 0, as this
	int keyI = 1;

	// Number of packets sent
	int packetSentCount = 0;

	FbxAnimStack *animStack = lScene->GetCurrentAnimationStack();
	FbxAnimLayer *animLayer = animStack->GetMember<FbxAnimLayer>();

	int keyTotal = getKeyCount(markerSet->GetChild(0), lScene);

	int childCount = markerSet->GetChildCount();

	// Variable used to alert the user about how many keys have already been processed
	int encodedKeyCount = keyI;
	while (keyI < keyTotal) {
		if ((((float)(keyI - encodedKeyCount)) / keyTotal) > 0.2) {
			encodedKeyCount = keyI;
			UI_Printf("%f%% of the keys have been encoded.", (((float)encodedKeyCount) / keyTotal) * 100);
		}
		
		// If vector is the requested size of latency window then shuffle it. 
		// If not add it to the vector keyIvec
		if (keyIvec.size() != p_latencyWindow) {
			keyIvec.push_back(keyI);
			//UI_Printf("%d has been added to the vector", keyI);
		}
		if (keyIvec.size() == p_latencyWindow || keyI == (keyTotal-1)) {
			
			// If Interleaving is enabled shuffle vector
			if (p_isInterleavingMode){
				std::random_shuffle(keyIvec.begin(), keyIvec.end());
			}
			for (std::vector<int>::iterator it = keyIvec.begin(); it != keyIvec.end(); ++it) {
				//UI_Printf("now encoding key: %d", *it);
				for (int ci = 0; ci < childCount; ci++) {
					int oldPIndex = pIndex;

					// Encode rotation curves
					pIndex = encodeKeyFrame(keyTotal, animLayer, markerSet->GetChild(ci), *it, out_buf, pIndex, s, false);
					if (oldPIndex == (Max_key_num - 1) && pIndex == 0) // packet has just been sent out
						packetSentCount++;

					oldPIndex = pIndex;
					// Encode translation curves - if they exist
					pIndex = encodeKeyFrame(keyTotal, animLayer, markerSet->GetChild(ci), *it, out_buf, pIndex, s, true);
					if (oldPIndex == (Max_key_num - 1) && pIndex == 0)
						packetSentCount++;


					// Sleep for a while, before sending more
					// TODO - fix concurrency issues. Make decoder faster
					Sleep(5);
			
				}
				
			}

			keyIvec.clear();

		}
		keyI++;
	}

	Sleep(100);
	if (pIndex > 0) {
		if (sendto(s,  out_buf, pIndex*fragmentSize, 0, (struct sockaddr *) &p_sock_addr, sizeof(p_sock_addr)) == SOCKET_ERROR)
		{
			UI_Printf("failed to send with error code : %d", WSAGetLastError());
		}
		packetSentCount++;
	}


	if (sendto(s, NULL, 0, 0, (struct sockaddr *) &p_sock_addr, sizeof(p_sock_addr)) == SOCKET_ERROR)
	{
		UI_Printf("failed to send with error code : %d", WSAGetLastError());
	}
	packetSentCount++;

	UI_Printf("Encoding has finished. %d packages were sent.", packetSentCount);

	delete out_buf;

	// Destroy scene
	lScene->Destroy();

	// Close socket after transmitting
	closesocket(s);



}


/// <summary>
/// Decode keyframes from a certain packet
/// </summary>
/// <param name="lScene">FBX Scene</param>
/// <param name="p">Raw data received from channel</param>
/// <param name="numBytesRecv">Number of bytes received</param>
void FBXCoding::decodePacket(FbxScene *lScene, char *p, int numBytesRecv) {


	// Retrieve animationlayer
	FbxAnimStack *animStack = lScene->GetCurrentAnimationStack();
	FbxAnimLayer *animLayer = animStack->GetMember<FbxAnimLayer>();

	// Calculate number of keyframes within packet
	int num_key_received;
	if (!isLDPCEnabled()) {
		num_key_received = numBytesRecv / sizeof(PACKET);
	}
	else {
		num_key_received = numBytesRecv / sizeof(PACKET_LDPC);
	}



	// Iterate on incoming packets
	for (int i = 0; i < num_key_received; i++) {
		if (!isLDPCEnabled()) {
			decodeFragment(animLayer, ((PACKET *)p)[i]);
		}
		else {
			decodeLDPCFragment(animLayer, ((PACKET_LDPC *)p)[i]);
		}
	}


}

/// <summary>
/// Decode LDPC packet fragment
/// </summary>
/// <param name="animLayer">FBX Animation layer</param>
/// <param name="jointMap">Map of joints and its corresponding identifiers</param>
/// <param name="frag">Fragment to be decoded</param>
void FBXCoding::decodeLDPCFragment(FbxAnimLayer *animLayer,  PACKET_LDPC &frag) {


	// Every LDPC_PACKET is also a PACKET, so we need to decode the rest of it
	decodeFragment(animLayer, frag);

	// Decode parity part
	p_ldpc_parity_map[std::pair<short, FbxLongLong>(frag.joint_id, frag.time)] = frag.bits;
	
}

/// <summary>
/// Decode packet fragment
/// </summary>
/// <param name="animLayer">FBX Animation layer</param>
/// <param name="jointMap">Map of joints and its corresponding identifiers</param>
/// <param name="frag">Fragment to be decoded</param>
void FBXCoding::decodeFragment(FbxAnimLayer *animLayer, PACKET &frag){

	//Extract frame rate
	if (frag.joint_id <= TRANSLATION_CUSTOM_ID) {
		if (p_fps == 0)  {
			p_fps = (int)abs(frag.joint_id);
			UI_Printf(" Motion Clip FPS is equal to %d", p_fps);
		}
		frag.joint_id = TRANSLATION_CUSTOM_ID;
	}

	auto it = p_jointMap.find(frag.joint_id);
	if (it == p_jointMap.end()){
		UI_Printf(" Decoding error. Unable to find node with id %d in the scene.", frag.joint_id);
		return;
	}

	FbxNode *tgtMarker = it->second;

	FbxAnimCurve *curveX, *curveY, *curveZ;
	FbxAnimCurveDef::EInterpolationType interpType;

	// Check what type of curve we received
	if (frag.joint_id == TRANSLATION_CUSTOM_ID) {
		curveX = tgtMarker->LclTranslation.GetCurve(animLayer, FBXSDK_CURVENODE_COMPONENT_X);
		curveY = tgtMarker->LclTranslation.GetCurve(animLayer, FBXSDK_CURVENODE_COMPONENT_Y);
		curveZ = tgtMarker->LclTranslation.GetCurve(animLayer, FBXSDK_CURVENODE_COMPONENT_Z);
		interpType = FbxAnimCurveDef::eInterpolationLinear;
	}
	else {
		curveX = tgtMarker->LclRotation.GetCurve(animLayer, FBXSDK_CURVENODE_COMPONENT_X);
		curveY = tgtMarker->LclRotation.GetCurve(animLayer, FBXSDK_CURVENODE_COMPONENT_Y);
		curveZ = tgtMarker->LclRotation.GetCurve(animLayer, FBXSDK_CURVENODE_COMPONENT_Z);
		interpType = FbxAnimCurveDef::eInterpolationCubic;
	}


	FbxTime kTime;
	kTime.SetMilliSeconds(frag.time);

	if (curveX) {
		curveX->KeyModifyBegin();

		// Start adding key
		// TODO: Speed up this operation using index
		int keyIndex = curveX->KeyInsert(kTime);
		curveX->KeySetValue(keyIndex, frag.x);
		curveX->KeySetInterpolation(keyIndex, interpType);

		curveX->KeyModifyEnd();
	}

	if (curveY) {
		curveY->KeyModifyBegin();

		// Start adding key
		// TODO: Speed up this operation using index
		int keyIndex = curveY->KeyInsert(kTime);
		curveY->KeySetValue(keyIndex, frag.y);
		curveY->KeySetInterpolation(keyIndex, interpType);

		curveY->KeyModifyEnd();
	}

	if (curveZ) {
		curveZ->KeyModifyBegin();

		// Start adding key
		// TODO: Speed up this operation using index
		int keyIndex = curveZ->KeyInsert(kTime);
		curveZ->KeySetValue(keyIndex, frag.z);
		curveZ->KeySetInterpolation(keyIndex, interpType);

		curveZ->KeyModifyEnd();
	}


}

/// <summary>
/// Encodes keys for curves from a given node at a given time
/// </summary>
/// <param name="animLayer">FBX Anim layer</param>
/// <param name="tgtNode">Node to have curves extracted</param>
/// <param name="keyIndex">Index of current key</param>
/// <param name="zCurve">Z curve</param>
/// <param name="p">Outgoing packet buffer</param>
/// <param name="pIndex">Index of current keyframe</param>
/// <param name="s">Socket used to send packages</param>
/// <param name="isTranslation">Are these translation curves?</param>
/// <return>Updated pIndex</return>
int FBXCoding::encodeKeyFrame(int keyTotal, FbxAnimLayer *animLayer, FbxNode *tgtNode, int keyIndex, char *p, int pIndex, SOCKET s, bool isTranslation) {


	FbxAnimCurve *xCurve;
	FbxAnimCurve *yCurve;
	FbxAnimCurve *zCurve;

	if (isTranslation) {
		xCurve = tgtNode->LclTranslation.GetCurve(animLayer, FBXSDK_CURVENODE_COMPONENT_X);
		yCurve = tgtNode->LclTranslation.GetCurve(animLayer, FBXSDK_CURVENODE_COMPONENT_Y);
		zCurve = tgtNode->LclTranslation.GetCurve(animLayer, FBXSDK_CURVENODE_COMPONENT_Z);
	}
	else {
		xCurve = tgtNode->LclRotation.GetCurve(animLayer, FBXSDK_CURVENODE_COMPONENT_X);
		yCurve = tgtNode->LclRotation.GetCurve(animLayer, FBXSDK_CURVENODE_COMPONENT_Y);
		zCurve = tgtNode->LclRotation.GetCurve(animLayer, FBXSDK_CURVENODE_COMPONENT_Z);
	}


	// Nothing to be processed, just leave
	if (!xCurve && !yCurve && !zCurve)
		return pIndex;

	// Calculate FPS
	if (p_fps == 0) {
		// Get any curve that is not null
		FbxAnimCurve *tgtCurve = (xCurve) ? xCurve : (yCurve) ? yCurve : zCurve;
		p_fps = computeFPS(tgtCurve);

		if (p_fps != 0)
			UI_Printf(" Motion Clip FPS is equal to %d", p_fps);
	}


	// Maximum number of keys a package can store
	int Max_key_num;
	int bytes_sent;
	PACKET *p_in_pIndex;

	if (p_enableLDPC) {
		PACKET_LDPC &p_at_pos = ((PACKET_LDPC *)p)[pIndex];
		Max_key_num = PACKET_SIZE / sizeof(PACKET_LDPC);
		bytes_sent = Max_key_num*sizeof(PACKET_LDPC);

		encodeCommonKeyAttributes(p_at_pos, keyIndex, tgtNode, xCurve, yCurve, zCurve, isTranslation);
		encodeLDPCAttributes(keyTotal, p_at_pos , xCurve, yCurve, zCurve, keyIndex);
	}
	else {
		PACKET &p_at_pos = ((PACKET *)p)[pIndex];
		Max_key_num = PACKET_SIZE / sizeof(PACKET);
		bytes_sent = Max_key_num*sizeof(PACKET);

		encodeCommonKeyAttributes(p_at_pos, keyIndex, tgtNode, xCurve, yCurve, zCurve, isTranslation);
	}
	
	// Increment index
	pIndex++;

	// Send data and clear the buffer
	if (pIndex >= Max_key_num) {
		if (sendto(s, (const char *)p, bytes_sent, 0, (struct sockaddr *) &p_sock_addr, sizeof(p_sock_addr)) == SOCKET_ERROR)
		{
			UI_Printf("failed to send with error code : %d", WSAGetLastError());
		}
		memset(p, NULL, Max_key_num);
		pIndex = 0;
	}

	// Return updated index
	return pIndex;
}


/// <summary>
/// Encodes Common key attributes
/// </summary>
/// <param name="animLayer">FBX Anim layer</param>
/// <param name="tgtNode">Node to have curves extracted</param>
/// <param name="p">Outgoing packet buffer</param>
/// <param name="pIndex">Index of current keyframe</param>
/// <param name="s">Socket used to send packages</param>
/// <param name="isTranslation">Are these translation curves?</param>
/// <return>Updated pIndex</return>
void FBXCoding::encodeCommonKeyAttributes(PACKET &outP, int keyIndex,  FbxNode *tgtNode, FbxAnimCurve *xCurve, FbxAnimCurve *yCurve, FbxAnimCurve *zCurve, bool isTranslation) {
	if (isTranslation) {
		short translationID = (p_fps != 0) ? TRANSLATION_CUSTOM_ID*p_fps : TRANSLATION_CUSTOM_ID;
		outP.joint_id = translationID;
	}
	else {
		outP.joint_id = getCustomIdProperty(tgtNode);
	}

	if (xCurve) {
		outP.x = xCurve->KeyGet(keyIndex).GetValue();
		outP.time = zCurve->KeyGet(keyIndex).GetTime().GetMilliSeconds();
	}
	if (yCurve) {
		outP.y = yCurve->KeyGet(keyIndex).GetValue();
		outP.time = zCurve->KeyGet(keyIndex).GetTime().GetMilliSeconds();

	}
	if (zCurve) {
		outP.z = zCurve->KeyGet(keyIndex).GetValue();
		outP.time = zCurve->KeyGet(keyIndex).GetTime().GetMilliSeconds();
	}

}

/// <summary>
/// Encodes LDPC parity
/// </summary>
void FBXCoding::encodeLDPCAttributes(int keyTotal, PACKET_LDPC &outP, FbxAnimCurve *xCurve, FbxAnimCurve *yCurve, FbxAnimCurve *zCurve, int keyIndex) {
	float x, y, z;
	itpp::LDPC_Code ldpc_encode(&H, &G);
	// Get X, Y, Z, for offset keyframe and encode them when LDPC is enabled



	int keyOffset = keyIndex + p_LDPC_offset;
	if (keyOffset < keyTotal) {
		//	UI_Printf("key offset of %d is: %d ", keyIndex, keyOffset);
		x = (xCurve)? xCurve->KeyGet(keyOffset).GetValue() :0.0;
		y = (yCurve)? yCurve->KeyGet(keyOffset).GetValue() :0.0;
		z = (zCurve)? zCurve->KeyGet(keyOffset).GetValue() :0.0;

		itpp::bvec bitsin = tobvec(x);
		bitsin = concat(bitsin, tobvec(y));
		bitsin = concat(bitsin, tobvec(z));

		itpp::bvec bitsout;
		ldpc_encode.encode(bitsin, bitsout);
		bvec2Bitset(bitsout, outP);

		//std::string bvecStringIn = itpp::to_str(bitsin);
		//UI_Printf("bitsin: %s", bvecStringIn.c_str());
	}
	// Printing the bits (checking to see if extracting the parity bits)
	/*
	std::string bvecStringIn = itpp::to_str(bitsin);
	std::string bvecStringOut = itpp::to_str(bitsout);

	UI_Printf("bitsin: %x", bvecStringIn.c_str());
	UI_Printf("bitsout: %s", bvecStringOut.c_str());
	UI_Printf("Parity bits: %s", ((PACKET_LDPC *) p)[pIndex].bits.to_string().c_str());
	*/
}

void FBXCoding::bvec2Bitset(itpp::bvec bin_list, PACKET_LDPC &p) {
	for (int i = 96; i < 104; i++) {
		p.bits[i%8] = bin_list[i];
	}
}

itpp::bvec FBXCoding::tobvec(float f) {
	union
	{
		float input;   // assumes sizeof(float) == sizeof(int)
		int   output;
	}    data;

	data.input = f;

	std::bitset<sizeof(float) * CHAR_BIT>   result(data.output);

	itpp::bvec resultBvec;
	resultBvec.set_length(sizeof(float) * CHAR_BIT);

	for (int i = 0; i < resultBvec.length(); i++) {
		resultBvec[i] = result[i];
	}

	return resultBvec;
}


/// <summary>
/// Converts bvec to float
/// </summary>
float FBXCoding::tofloat(itpp::bvec &input) {

	std::bitset<sizeof(float) * CHAR_BIT>   tempBSet;

	for (int i = 0; i < input.length(); i++) {
		tempBSet[i] = input[i];
	}

	union
	{
		unsigned long input;   // assumes sizeof(float) == sizeof(int)
		float   output;
	}    data;


	data.input = tempBSet.to_ulong();
	return data.output;

}

/// <summary>
/// Converts parity bitset to bvec
/// </summary>
itpp::bvec FBXCoding::tobvec(std::bitset<N_PARITY_BIT> bset) {

	itpp::bvec resultBvec;
	resultBvec.set_length(N_PARITY_BIT);

	for (int i = 0; i < resultBvec.length(); i++) {
		resultBvec[i] = bset[i];
	}

	return resultBvec;

}
/// <summary>
/// Recovers missing information in the animation by using LDPC parity data
/// </summary>
void FBXCoding::startLDPCRecovery(FbxScene *lScene) {


	if (!isLDPCEnabled())
		return;

	UI_Printf("LDPC is enabled. Starting recovery of missing packets.");



	if (p_fps == 0) {
		UI_Printf(" LDPC Reconstruction failed: Unable to estimate FPS.");
		return;
	}

	FbxAnimStack *animStack = lScene->GetCurrentAnimationStack();
	FbxAnimLayer *animLayer = animStack->GetMember<FbxAnimLayer>();


	for (auto &it : p_ldpc_parity_map) {

		auto &key_pair = it.first;
		auto &value_parity = it.second;

		auto &node_it = p_jointMap.find(key_pair.first);
		if (node_it == p_jointMap.end())
			continue;

		FbxNode *tgtNode = node_it->second;

		FbxAnimCurve *xCurve, *yCurve, *zCurve, *tgtCurve = NULL;

		if (key_pair.first == TRANSLATION_CUSTOM_ID) {
			xCurve = tgtNode->LclTranslation.GetCurve(animLayer, FBXSDK_CURVENODE_COMPONENT_X);
			yCurve = tgtNode->LclTranslation.GetCurve(animLayer, FBXSDK_CURVENODE_COMPONENT_Y);
			zCurve = tgtNode->LclTranslation.GetCurve(animLayer, FBXSDK_CURVENODE_COMPONENT_Z);
		}
		else {
			xCurve = tgtNode->LclRotation.GetCurve(animLayer, FBXSDK_CURVENODE_COMPONENT_X);
			yCurve = tgtNode->LclRotation.GetCurve(animLayer, FBXSDK_CURVENODE_COMPONENT_Y);
			zCurve = tgtNode->LclRotation.GetCurve(animLayer, FBXSDK_CURVENODE_COMPONENT_Z);
		}

		// Target key time
		FbxTime keyTime;
		FbxLongLong offsetTime = computeOffsetTime(key_pair.second, p_LDPC_offset, p_fps);
		keyTime.SetMilliSeconds(offsetTime);

		// X,Y and Z values extracted from curve
		float xInterpVal = 0.0, yInterpVal = 0.0, zInterpVal = 0.0;

		if (xCurve) {
			xInterpVal = xCurve->Evaluate(keyTime);
			tgtCurve = xCurve;
		}
		if (yCurve) {
			yInterpVal = yCurve->Evaluate(keyTime);
			tgtCurve = yCurve;
		}
		if (zCurve) {
			yInterpVal = zCurve->Evaluate(keyTime);
			tgtCurve = zCurve;
		}


		if (tgtCurve == NULL)
			continue;
		

		// Offset time 
		double temp;
		double keyIndex = tgtCurve->KeyFind(key_pair.second);
		// There is no key for the given time - reconstruct at that point
		if (modf(keyIndex, &temp) > c_minKeyIndexDiff ) {
			UI_Printf("Reconstructing index %f", keyIndex);
				
			itpp::bvec encodedVec = encodeCurveLDPC(xInterpVal, yInterpVal, zInterpVal, value_parity);
			static itpp::LDPC_Code decoder(&H, &G);
			itpp::bvec decodedVec = decoder.decode(encodedVec);


			itpp::bvec zVec = decodedVec.split(sizeof(float) * 8);
			itpp::bvec yVec = decodedVec.split(sizeof(float) * 8);
			itpp::bvec xVec = decodedVec;

			float zNewVal = tofloat(zVec), yNewVal = tofloat(yVec), xNewVal = tofloat(xVec);

			

			// Create new keys
			if (xCurve) {
				insertKeyCurve(xCurve, keyTime, xNewVal, (key_pair.first == TRANSLATION_CUSTOM_ID));
			}
			if (yCurve) {
				insertKeyCurve(yCurve, keyTime, yNewVal, (key_pair.first == TRANSLATION_CUSTOM_ID));
			}
			if (zCurve) {
				insertKeyCurve(zCurve, keyTime, zNewVal, (key_pair.first == TRANSLATION_CUSTOM_ID));
			}
		}
	}


	UI_Printf("LDPC recovery has been performed.");
}


/// <summary>
/// Encode curve data and parity, so we can use LDPC to decode it and fix missing values
/// </summary>
itpp::bvec FBXCoding::encodeCurveLDPC(float xIntVal, float yIntVal, float zIntVal, std::bitset<N_PARITY_BIT> &parityVal) {

	itpp::bvec xVec = tobvec(xIntVal);
	itpp::bvec yVec = tobvec(yIntVal);
	itpp::bvec zVec = tobvec(zIntVal);
	itpp::bvec parityVec = tobvec(parityVal);


	itpp::bvec encodedVec = itpp::concat(xVec, yVec, zVec, parityVec);

	return encodedVec;
}