#include "JSystem/JAudio2/JASBNKParser.hpp"
#include "JSystem/JAudio2/JASBasicBank.hpp"
#include "JSystem/JAudio2/JASBasicInst.hpp"
#include "JSystem/JAudio2/JASCalc.hpp"
#include "JSystem/JAudio2/JASChannel.hpp"
#include "JSystem/JAudio2/JASDrumSet.hpp"
#include "JSystem/JAudio2/JASGadget.hpp"
#include "JSystem/JAudio2/JASHeapCtrl.hpp"
#include "JSystem/JAudio2/JASInstRand.hpp"
#include "JSystem/JAudio2/JASInstSense.hpp"
#include "JSystem/JKernel/JKRSolidHeap.hpp"
#include "JSystem/JSupport/JSupport.hpp"

#ifdef PETARI_NATIVE
// These structures overlay disc data and must keep the Wii layout.
static_assert(sizeof(JASBNKParser::Ver1::TOsc) == 0x1C, "Ver1::TOsc layout");
static_assert(sizeof(JASBNKParser::Ver1::TPercData) == 0x10, "Ver1::TPercData layout");
static_assert(sizeof(JASBNKParser::Ver1::TVeloData) == 0x10, "Ver1::TVeloData layout");
static_assert(sizeof(JASBNKParser::Ver0::TInst) == 0x2C, "Ver0::TInst layout");
static_assert(sizeof(JASBNKParser::Ver0::TPerc) == 0x408, "Ver0::TPerc layout");
static_assert(sizeof(JASBNKParser::Ver0::THeader) == 0x3E4, "Ver0::THeader layout");
#endif

template < typename T >
inline T readStream(void const* stream, u32 offset) {
    return *(T*)((intptr_t)stream + offset);
}

// Oscillator point tables are copied out of the bank and read by the oscillator
// at runtime, so native copies are converted to host order.
static inline void JASBNKPointsToHost(void* pTable, u32 size) {
#ifdef PETARI_NATIVE
    JSU_BE(s16)* src = static_cast< JSU_BE(s16)* >(pTable);
    s16* dst = static_cast< s16* >(pTable);
    for (u32 i = 0; i < size / sizeof(s16); i++) {
        dst[i] = src[i];
    }
#endif
}

JASBank* JASBNKParser::createBank(void const* stream, JKRHeap* heap) {
    return createBasicBank(stream, heap);
}

u32 JASBNKParser::sUsedHeapSize;

JASBasicBank* JASBNKParser::createBasicBank(void const* stream, JKRHeap* heap) {
    if (heap == nullptr) {
        heap = JASDram;
    }

    u32 free_size = heap->getFreeSize();

    TFileHeader* filep = (TFileHeader*)stream;
    JASBasicBank* bank = nullptr;
    switch (filep->mVersion) {
    case 0:
        bank = Ver0::createBasicBank(stream, heap);
        break;
    case 1:
        bank = Ver1::createBasicBank(stream, heap);
        break;
    }

    sUsedHeapSize += free_size - heap->getFreeSize();
    return bank;
}

JASBNKParser::Ver1::TChunk* JASBNKParser::Ver1::findChunk(void const* stream, u32 id) {
    TFileHeader* header = (TFileHeader*)stream;
    void* end = (void*)((intptr_t)stream + header->mSize);
    TChunk* chunk = (TChunk*)((intptr_t)stream + 0x20);
    while (chunk < end) {
        if (chunk->mID == id) {
            return chunk;
        }
        chunk = (TChunk*)(((intptr_t)chunk + 0xb + chunk->mSize) & ~3);
    }
    return nullptr;
}

JASInstEffect* JASBNKParser::Ver1::createEffector(void const* stream, JKRHeap* heap) {
    const JSU_BE(u32)* data = (const JSU_BE(u32)*)(intptr_t)stream;
    switch (*data++) {
    case 'Rand': {
        TRandData* randData = (TRandData*)data;
        JASInstRand* rand = new (heap, 0) JASInstRand();
        rand->mType = randData->mType;
        rand->setBase(randData->mBase);
        rand->setRange(randData->mRange);
        return rand;
    }
    case 'Sens': {
        TSenseData* senseData = (TSenseData*)data;
        JASInstSense* sense = new (heap, 0) JASInstSense();
        sense->mType = senseData->mType;
        sense->setParams(senseData->mSenseType, senseData->mMaxPoint, senseData->mStartLvl, senseData->mEndLvl);
        return sense;
    }
    default:
        return nullptr;
    }
}

JASBasicBank* JASBNKParser::Ver1::createBasicBank(void const* stream, JKRHeap* heap) {
    if (heap == nullptr) {
        heap = JASDram;
    }

    JASBasicBank* bank = new (heap, 0) JASBasicBank();
    if (bank == nullptr) {
        return nullptr;
    }

    TEnvtChunk* envt_chunk = (TEnvtChunk*)findChunk(stream, 'ENVT');
    TOscChunk* osc_chunk = (TOscChunk*)findChunk(stream, 'OSCT');
    TListChunk* list_chunk = (TListChunk*)findChunk(stream, 'LIST');

    u8* envt = new (heap, 2) u8[envt_chunk->mSize];
    JASCalc::bcopy(envt_chunk->mData, envt, envt_chunk->mSize);
    JASBNKPointsToHost(envt, envt_chunk->mSize);

    JSU_BE(u32)* ptr = &osc_chunk->mCount;
    u32 count = *ptr++;
    JASOscillator::Data* osc_data = new (heap, 0) JASOscillator::Data[count];
    for (int i = 0; i < count; i++, ptr += sizeof(TOsc) >> 2) {
        TOsc* op = (TOsc*)ptr;
        JASOscillator::Data* data = &osc_data[i];
        data->mTarget = op->mTarget;
        data->_04 = op->_08;
        data->mScale = op->mScale;
        data->_14 = op->_18;
        data->mTable = (JASOscillator::Point*)(envt + op->mTableOffset);
        data->rel_table = (JASOscillator::Point*)(envt + op->_10);
    }

    TListChunk* list = list_chunk;
    for (int i = 0; i < list->count; i++) {
        if (list->mOffsets[i] != 0) {
            JSU_BE(u32)* data = (JSU_BE(u32)*)((intptr_t)stream + list->mOffsets[i]);
            switch (*data++) {
            case 'Inst': {
                JASBasicInst* instp = new (heap, 0) JASBasicInst();
                u32 oscCount = *data++;
                for (int j = 0; j < oscCount; j++) {
                    u32 index = *data++;
                    instp->setOsc(j, &osc_data[index]);
                }

                u32 effectCount = *data++;
                for (int j = 0; j < effectCount; j++) {
                    JASInstEffect* effect = createEffector((void*)((intptr_t)stream + *data++), heap);
                    instp->appendEffect(new (heap, 0) JASPtrListAdaptor< JASInstEffect >(effect));
                }

                u32 keyRegionCount = *data++;
                instp->setKeyRegionCount(keyRegionCount, heap);
                for (int j = 0; j < keyRegionCount; j++) {
                    JASBasicInst::TKeymap* keymap = instp->getKeyRegion(j);
                    keymap->setHighKey(data[0] >> 0x18);
                    u32 veloCount = data[1];
                    data += 2;
                    keymap->setVeloRegionCount(veloCount, heap);
                    for (int k = 0; k < veloCount; k++) {
                        JASBasicInst::TVelomap* velomap = keymap->getVeloRegion(k);
                        TVeloData* veloData = (TVeloData*)data;
                        velomap->_0 = veloData->_0;
                        velomap->_4 = (u16)veloData->_4;
                        velomap->mVolume = veloData->mVolume;
                        velomap->mPitch = veloData->mPitch;
                        data += 4;
                    }
                }
                TInstData* instData = (TInstData*)data;
                instp->setVolume(instData->mVolume);
                instp->setPitch(instData->mPitch);
                bank->setInst(i, instp);
                break;
            }

            case 'Perc': {
                JASDrumSet* drump = new (heap, 0) JASDrumSet();
                u32 pmap_count = data[1];
                u32 count = *data++;
                for (int j = 0; j < count; j++) {
                    u32 offset = *data++;
                    if (offset != 0) {
                        JASDrumSet::TPerc* percp = new (heap, 0) JASDrumSet::TPerc();
                        JSU_BE(u32)* ptr = (JSU_BE(u32)*)((intptr_t)stream + offset);
                        TPercData* perc_data = (TPercData*)(ptr + 1);
                        percp->setVolume(perc_data->mVolume);
                        percp->setPitch(perc_data->mPitch);
                        percp->setPan((f32)perc_data->mPan / 127.0f);
                        percp->setRelease(perc_data->mRelease);

                        ptr += 4;
                        u32 effectCount = *ptr++;
                        for (u32 j = 0; j < effectCount; j++) {
                            JASInstEffect* effect = createEffector((void*)((intptr_t)stream + *ptr++), heap);
                            percp->appendEffect(new (heap, 0) JASPtrListAdaptor< JASInstEffect >(effect));
                        }

                        u32 veloCount = *ptr++;
                        percp->setVeloRegionCount(veloCount, heap);
                        for (int k = 0; k < veloCount; k++) {
                            JASDrumSet::TVelomap* velomap = percp->getVeloRegion(k);
                            TVeloData* veloData = (TVeloData*)ptr;
                            velomap->_0 = veloData->_0;
                            velomap->_4 = (u16)veloData->_4;
                            velomap->mVolume = veloData->mVolume;
                            velomap->mPitch = veloData->mPitch;
                            ptr += 4;
                        }
                        drump->setPerc(j, percp);
                    }
                }
                bank->setInst(i, drump);
                break;
            }
            }
        }
    }

    return bank;
}

inline JASInstParam::JASInstParam() {
    init();
}

JASBasicBank* JASBNKParser::Ver0::createBasicBank(void const* stream, JKRHeap* heap) {
    if (heap == nullptr) {
        heap = JASDram;
    }

    THeader const* header = (THeader*)stream;
    JASBasicBank* bank = new (heap, 0) JASBasicBank();
    if (bank == nullptr) {
        return nullptr;
    }

    for (int i = 0; i < 0x80; i++) {
        TInst* tinst = header->mOffsets.mInstOffset[i].ptr(header);
        if (tinst != nullptr) {
            JASBasicInst* instp = new (heap, 0) JASBasicInst();
            instp->setVolume(tinst->mVolume);
            instp->setPitch(tinst->mPitch);

            int osc_idx = 0;
            for (int j = 0; j < 2; j++) {
                TOsc* tosc = tinst->mOscOffset[j].ptr(header);
                if (tosc != nullptr) {
                    JASOscillator::Data* osc = findOscPtr(bank, header, tosc);

                    if (osc != nullptr) {
                        instp->setOsc(osc_idx, osc);
                    } else {
                        osc = new (heap, 0) JASOscillator::Data();
                        osc->mTarget = tosc->mTarget;
                        osc->_04 = tosc->field_0x4;

                        JASOscillator::Point* points = tosc->mPointOffset.ptr(header);
                        if (points != nullptr) {
                            const JASOscillator::Point* endPtr = getOscTableEndPtr(points);
                            int size = endPtr - points;
                            JASOscillator::Point* table = new (heap, 0) JASOscillator::Point[size];
                            JASCalc::bcopy(points, table, size * sizeof(JASOscillator::Point));
                            JASBNKPointsToHost(table, size * sizeof(JASOscillator::Point));
                            osc->mTable = table;
                        } else {
                            osc->mTable = nullptr;
                        }

                        points = tosc->field_0xc.ptr(header);
                        if (points != nullptr) {
                            const JASOscillator::Point* endPtr = getOscTableEndPtr(points);
                            int size = endPtr - points;
                            JASOscillator::Point* table = new (heap, 0) JASOscillator::Point[size];
                            JASCalc::bcopy(points, table, size * sizeof(JASOscillator::Point));
                            JASBNKPointsToHost(table, size * sizeof(JASOscillator::Point));
                            osc->rel_table = table;
                        } else {
                            osc->rel_table = nullptr;
                        }

                        osc->mScale = tosc->mScale;
                        osc->_14 = tosc->field_0x14;
                        instp->setOsc(osc_idx, osc);
                    }

                    osc_idx++;
                }
            }

            for (int j = 0; j < 2; j++) {
                TRandEffect* randEffect = tinst->mRandOffset[j].ptr(header);
                if (randEffect != nullptr) {
                    JASInstRand* rand = new (heap, 0) JASInstRand();
                    rand->mType = randEffect->mType;
                    rand->mBaseValue = randEffect->mBaseValue;
                    rand->mRange = randEffect->mRange;
                    instp->appendEffect(new (heap, 0) JASPtrListAdaptor< JASInstEffect >(rand));
                }
            }

            for (int j = 0; j < 2; j++) {
                TSenseEffect* senseEffect = tinst->mSenseOffset[j].ptr(header);
                if (senseEffect != nullptr) {
                    JASInstSense* sense = new (heap, 0) JASInstSense();
                    sense->mType = senseEffect->mType;
                    sense->setParams(senseEffect->mSenseType, senseEffect->mMaxPoint, senseEffect->mStartLvl, senseEffect->mEndLvl);
                    instp->appendEffect(new (heap, 0) JASPtrListAdaptor< JASInstEffect >(sense));
                }
            }

            instp->setKeyRegionCount(tinst->mKeyRegionCount, heap);
            for (int j = 0; j < tinst->mKeyRegionCount; j++) {
                JASBasicInst::TKeymap* keymap = instp->getKeyRegion(j);
                TKeymap* tkeymap = tinst->mKeymapOffset[j].ptr(header);
                keymap->setHighKey(tkeymap->mHighKey);

                keymap->setVeloRegionCount(tkeymap->mVeloRegionCount, heap);
                for (int k = 0; k < tkeymap->mVeloRegionCount; k++) {
                    JASBasicInst::TVelomap* velomap = keymap->getVeloRegion(k);
                    TVmap* tvmap = tkeymap->mVmapOffset[k].ptr(header);

                    velomap->_0 = tvmap->_0;
                    velomap->_4 = tvmap->_4 & 0xFFFF;
                    velomap->mVolume = tvmap->mVolume;
                    velomap->mPitch = tvmap->mPitch;
                }
            }

            bank->setInst(i, instp);
        }
    }

    for (int i = 0; i < 12; i++) {
        TPerc* tperc = header->mOffsets.mPercOffset[i].ptr(header);
        if (tperc != nullptr) {
            JASDrumSet* setp = new (heap, 0) JASDrumSet();

            for (int j = 0; j < 0x80; j++) {
                TPmap* tpmap = tperc->mPmapOffset[j].ptr(header);
                if (tpmap != nullptr) {
                    JASDrumSet::TPerc* percp = new (heap, 0) JASDrumSet::TPerc();
                    percp->setVolume(tpmap->mVolume);
                    percp->setPitch(tpmap->mPitch);
                    if (tperc->mMagic == 'PER2') {
                        percp->setPan(tperc->mPan[j] / 127.0f);
                        percp->setRelease(tperc->mRelease[j]);
                    }

                    for (int k = 0; k < 2; k++) {
                        TRandEffect* randEffect = tpmap->mRandOffset[k].ptr(header);
                        if (randEffect != nullptr) {
                            JASInstRand* rand = new (heap, 0) JASInstRand();
                            rand->mType = randEffect->mType;
                            rand->mBaseValue = randEffect->mBaseValue;
                            rand->mRange = randEffect->mRange;
                            percp->appendEffect(new (heap, 0) JASPtrListAdaptor< JASInstEffect >(rand));
                        }
                    }

                    percp->setVeloRegionCount(tpmap->mVeloRegionCount, heap);
                    for (int k = 0; k < tpmap->mVeloRegionCount; k++) {
                        JASDrumSet::TVelomap* velomap = percp->getVeloRegion(k);
                        TVmap* tvmap = tpmap->mVmapOffset[k].ptr(header);

                        velomap->_0 = tvmap->_0;
                        velomap->_4 = tvmap->_4 & 0xFFFF;
                        velomap->mVolume = tvmap->mVolume;
                        velomap->mPitch = tvmap->mPitch;
                    }

                    setp->setPerc(j, percp);
                }
            }

            bank->setInst(i + 0xe4, setp);
        }
    }

    return bank;
}

JASOscillator::Data* JASBNKParser::Ver0::findOscPtr(JASBasicBank* bank, THeader const* header, TOsc const* osc) {
    TOffsetData const* data = &header->mOffsets;
    for (int i = 0; i < 0x80; i++) {
        TInst* tinst = data->mInstOffset[i].ptr(header);
        if (tinst != nullptr) {
            for (int j = 0; j < 2; j++) {
                if (tinst->mOscOffset[j].ptr(header) == osc) {
                    JASInst* inst = bank->getInst(i);
                    if (inst != nullptr) {
                        JASInstParam param;
                        inst->getParam(0x3c, 0x7f, &param);
                        if (j < param.mNumOscillators) {
                            return param.mOscillatorData[j];
                        }
                    }
                }
            }
        }
    }
    return nullptr;
}

JASOscillator::Point const* JASBNKParser::Ver0::getOscTableEndPtr(JASOscillator::Point const* points) {
    const JASOscillator::Point* ptr = points;
    while (true) {
        // Reads the serialized (big-endian) table.
        s16 tmp = *reinterpret_cast< const JSU_BE(s16)* >(ptr);
        ptr++;
        if (tmp > 10) {
            break;
        }
    }
    return ptr;
}
