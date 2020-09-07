/* -----------------------------------------------------------------------------
Software Copyright License for the Fraunhofer Software Library VVenc

(c) Copyright (2019-2020) Fraunhofer-Gesellschaft zur Förderung der angewandten Forschung e.V. 

1.    INTRODUCTION

The Fraunhofer Software Library VVenc (“Fraunhofer Versatile Video Encoding Library”) is software that implements (parts of) the Versatile Video Coding Standard - ITU-T H.266 | MPEG-I - Part 3 (ISO/IEC 23090-3) and related technology. 
The standard contains Fraunhofer patents as well as third-party patents. Patent licenses from third party standard patent right holders may be required for using the Fraunhofer Versatile Video Encoding Library. It is in your responsibility to obtain those if necessary. 

The Fraunhofer Versatile Video Encoding Library which mean any source code provided by Fraunhofer are made available under this software copyright license. 
It is based on the official ITU/ISO/IEC VVC Test Model (VTM) reference software whose copyright holders are indicated in the copyright notices of its source files. The VVC Test Model (VTM) reference software is licensed under the 3-Clause BSD License and therefore not subject of this software copyright license.

2.    COPYRIGHT LICENSE

Internal use of the Fraunhofer Versatile Video Encoding Library, in source and binary forms, with or without modification, is permitted without payment of copyright license fees for non-commercial purposes of evaluation, testing and academic research. 

No right or license, express or implied, is granted to any part of the Fraunhofer Versatile Video Encoding Library except and solely to the extent as expressly set forth herein. Any commercial use or exploitation of the Fraunhofer Versatile Video Encoding Library and/or any modifications thereto under this license are prohibited.

For any other use of the Fraunhofer Versatile Video Encoding Library than permitted by this software copyright license You need another license from Fraunhofer. In such case please contact Fraunhofer under the CONTACT INFORMATION below.

3.    LIMITED PATENT LICENSE

As mentioned under 1. Fraunhofer patents are implemented by the Fraunhofer Versatile Video Encoding Library. If You use the Fraunhofer Versatile Video Encoding Library in Germany, the use of those Fraunhofer patents for purposes of testing, evaluating and research and development is permitted within the statutory limitations of German patent law. However, if You use the Fraunhofer Versatile Video Encoding Library in a country where the use for research and development purposes is not permitted without a license, you must obtain an appropriate license from Fraunhofer. It is Your responsibility to check the legal requirements for any use of applicable patents.    

Fraunhofer provides no warranty of patent non-infringement with respect to the Fraunhofer Versatile Video Encoding Library.


4.    DISCLAIMER

The Fraunhofer Versatile Video Encoding Library is provided by Fraunhofer "AS IS" and WITHOUT ANY EXPRESS OR IMPLIED WARRANTIES, including but not limited to the implied warranties fitness for a particular purpose. IN NO EVENT SHALL FRAUNHOFER BE LIABLE for any direct, indirect, incidental, special, exemplary, or consequential damages, including but not limited to procurement of substitute goods or services; loss of use, data, or profits, or business interruption, however caused and on any theory of liability, whether in contract, strict liability, or tort (including negligence), arising in any way out of the use of the Fraunhofer Versatile Video Encoding Library, even if advised of the possibility of such damage.

5.    CONTACT INFORMATION

Fraunhofer Heinrich Hertz Institute
Attention: Video Coding & Analytics Department
Einsteinufer 37
10587 Berlin, Germany
www.hhi.fraunhofer.de/vvc
vvc@hhi.fraunhofer.de
----------------------------------------------------------------------------- */


/** \file     EncCu.cpp
    \brief    Coding Unit (CU) encoder class
*/

#include "EncCu.h"
#include "EncLib.h"
#include "Analyze.h"
#include "EncPicture.h"
#include "EncModeCtrl.h"
#include "BitAllocation.h"

#include "CommonLib/dtrace_codingstruct.h"
#include "CommonLib/Picture.h"
#include "CommonLib/UnitTools.h"
#include "CommonLib/dtrace_buffer.h"
#include "CommonLib/TimeProfiler.h"

#include <mutex>
#include <cmath>
#include <algorithm>

//! \ingroup EncoderLib
//! \{

namespace vvenc {

const uint8_t EncCu::m_GeoModeTest[GEO_MAX_NUM_CANDS][2] = { {0, 1}, {1, 0}, {0, 2}, {1, 2}, {2, 0}, {2, 1}, {0, 3}, {1, 3},
                                                             {2, 3}, {3, 0}, {3, 1}, {3, 2}, {0, 4}, {1, 4}, {2, 4}, {3, 4},
                                                             {4, 0}, {4, 1}, {4, 2}, {4, 3}, {0, 5}, {1, 5}, {2, 5}, {3, 5},
                                                             {4, 5}, {5, 0}, {5, 1}, {5, 2}, {5, 3}, {5, 4} };
// ====================================================================================================================
EncCu::EncCu()
  : m_CtxCache          ( nullptr )
  , m_globalCtuQpVector ( nullptr )
  , m_wppMutex          ( nullptr )
  , m_rcMutex           ( nullptr )
  , m_CABACEstimator    ( nullptr )
{
}

void EncCu::initPic( Picture* pic )
{
  const ReshapeData& reshapeData = pic->reshapeData;
  m_cRdCost.setReshapeParams( reshapeData.getReshapeLumaLevelToWeightPLUT(), reshapeData.getChromaWeight() );
  m_cInterSearch.setSearchRange( pic->cs->slice, *m_pcEncCfg );

  m_wppMutex = m_pcEncCfg->m_numWppThreads ? &pic->wppMutex : nullptr;
}

void EncCu::initSlice( const Slice* slice )
{
  m_cTrQuant.setLambdas( slice->getLambdas() );
  m_cRdCost.setLambda( slice->getLambdas()[0], slice->sps->bitDepths );
}

void EncCu::setCtuEncRsrc( CABACWriter* cabacEstimator, CtxCache* ctxCache, ReuseUniMv* pReuseUniMv, BlkUniMvInfoBuffer* pBlkUniMvInfoBuffer, AffineProfList* pAffineProfList)
{
  m_CABACEstimator = cabacEstimator;
  m_CtxCache       = ctxCache;
  m_cIntraSearch.setCtuEncRsrc( cabacEstimator, ctxCache );
  m_cInterSearch.setCtuEncRsrc( cabacEstimator, ctxCache, pReuseUniMv, pBlkUniMvInfoBuffer, pAffineProfList );
}

void EncCu::setUpLambda (Slice& slice, const double dLambda, const int iQP, const bool setSliceLambda, const bool saveUnadjusted, const bool useRC)
{
  if ( useRC )
  {
    m_cRdCost.setDistortionWeight( COMP_Y, 1.0 );
  }
  // store lambda
  m_cRdCost.setLambda( dLambda, slice.sps->bitDepths );

  // for RDO
  // in RdCost there is only one lambda because the luma and chroma bits are not separated, instead we weight the distortion of chroma.
  double dLambdas[MAX_NUM_COMP] = { dLambda };
  for( uint32_t compIdx = 1; compIdx < MAX_NUM_COMP; compIdx++ )
  {
    const ComponentID compID = ComponentID( compIdx );
    int chromaQPOffset       = slice.pps->chromaQpOffset[compID] + slice.sliceChromaQpDelta[ compID ];
    int qpc = slice.sps->chromaQpMappingTable.getMappedChromaQpValue(compID, iQP) + chromaQPOffset;
    double tmpWeight         = pow( 2.0, ( iQP - qpc ) / 3.0 );  // takes into account of the chroma qp mapping and chroma qp Offset
    if( m_pcEncCfg->m_DepQuantEnabled/* && !( m_pcEncCfg->getLFNST() ) */)
    {
      tmpWeight *= ( m_pcEncCfg->m_GOPSize >= 8 ? pow( 2.0, 0.1/3.0 ) : pow( 2.0, 0.2/3.0 ) );  // increase chroma weight for dependent quantization (in order to reduce bit rate shift from chroma to luma)
    }
    m_cRdCost.setDistortionWeight( compID, tmpWeight );
    dLambdas[compIdx] = dLambda / tmpWeight;
  }

  // for RDOQ
  m_cTrQuant.setLambdas( dLambdas );

  // for SAO, ALF
  if (setSliceLambda)
  {
    slice.setLambdas( dLambdas );
  }
  if( saveUnadjusted )
  {
    m_cRdCost.saveUnadjustedLambda();
  }
}

void EncCu::updateLambda(const Slice& slice, const double ctuLambda, const int ctuQP, const int newQP, const bool saveUnadjusted)
{
  const double  corrFactor = pow (2.0, double (newQP - ctuQP) / 3.0);
  const double  newLambda  = ctuLambda * corrFactor;
  const double* oldLambdas = slice.getLambdas(); // assumes prior setUpLambda (slice, ctuLambda) call!
  const double  newLambdas[MAX_NUM_COMP] = { oldLambdas[COMP_Y] * corrFactor, oldLambdas[COMP_Cb] * corrFactor, oldLambdas[COMP_Cr] * corrFactor };

  m_cTrQuant.setLambdas ( newLambdas);
  m_cRdCost.setLambda   ( newLambda, slice.sps->bitDepths);

  if (saveUnadjusted)
  {
    m_cRdCost.saveUnadjustedLambda(); // TODO hlm: check if this actually improves the overall quality
  }
}

void EncCu::init( const EncCfg& encCfg, const SPS& sps, LoopFilter* LoopFilter,
                  std::vector<int>* const globalCtuQpVector, Ctx* syncPicCtx, RateCtrl* pRateCtrl )
{
  DecCu::init( &m_cTrQuant, &m_cIntraSearch, &m_cInterSearch, encCfg.m_internChromaFormat );
  m_cRdCost.create     ();
  m_cRdCost.setCostMode( encCfg.m_costMode );
  if ( encCfg.m_lumaReshapeEnable || encCfg.m_lumaLevelToDeltaQPEnabled )
  {
    m_cRdCost.setReshapeInfo( encCfg.m_lumaReshapeEnable ? encCfg.m_reshapeSignalType : RESHAPE_SIGNAL_PQ, encCfg.m_internalBitDepth[ CH_L ], encCfg.m_internChromaFormat );
  }

  m_modeCtrl.init     ( encCfg, &m_cRdCost );
  m_cIntraSearch.init ( encCfg, &m_cTrQuant, &m_cRdCost, &m_SortedPelUnitBufs, m_unitCache );
  m_cInterSearch.init ( encCfg, &m_cTrQuant, &m_cRdCost, &m_modeCtrl, m_cIntraSearch.getSaveCSBuf() );
  m_cTrQuant.init     ( nullptr, encCfg.m_RDOQ, encCfg.m_useRDOQTS, encCfg.m_useSelectiveRDOQ, true, false /*m_useTransformSkipFast*/, encCfg.m_dqThresholdVal );

  m_syncPicCtx = syncPicCtx;                         ///< context storage for state of contexts at the wavefront/WPP/entropy-coding-sync second CTU of tile-row used for estimation
  m_pcRateCtrl = pRateCtrl;

  m_rcMutex = encCfg.m_numWppThreads ? &m_pcRateCtrl->rcMutex : nullptr;

  // Initialise scaling lists: The encoder will only use the SPS scaling lists. The PPS will never be marked present.
  const int maxLog2TrDynamicRange[ MAX_NUM_CH ] = { sps.getMaxLog2TrDynamicRange( CH_L ), sps.getMaxLog2TrDynamicRange( CH_C ) };
  m_cTrQuant.getQuant()->setFlatScalingList( maxLog2TrDynamicRange, sps.bitDepths );

  m_pcEncCfg       = &encCfg;
  m_pcLoopFilter   = LoopFilter;

  m_GeoCostList.init(GEO_NUM_PARTITION_MODE, encCfg.m_maxNumGeoCand );
  m_AFFBestSATDCost = MAX_DOUBLE;

  unsigned      uiMaxSize    = encCfg.m_CTUSize;
  ChromaFormat  chromaFormat = encCfg.m_internChromaFormat;

  const unsigned maxSizeIdx  = MAX_CU_SIZE_IDX;
  m_pTempCS = new CodingStructure**  [maxSizeIdx];
  m_pBestCS = new CodingStructure**  [maxSizeIdx];
  m_pTempCS2 = new CodingStructure** [maxSizeIdx];
  m_pBestCS2 = new CodingStructure** [maxSizeIdx];
  m_pOrgBuffer = new PelStorage**    [maxSizeIdx];
  m_pRspBuffer = new PelStorage**    [maxSizeIdx];

  for( unsigned wIdx = 0; wIdx < maxSizeIdx; wIdx++ )
  {
    m_pTempCS[wIdx] = new CodingStructure*  [maxSizeIdx];
    m_pBestCS[wIdx] = new CodingStructure*  [maxSizeIdx];
    m_pTempCS2[wIdx] = new CodingStructure* [maxSizeIdx];
    m_pBestCS2[wIdx] = new CodingStructure* [maxSizeIdx];
    m_pOrgBuffer[wIdx] = new PelStorage*    [maxSizeIdx];
    m_pRspBuffer[wIdx] = new PelStorage*    [maxSizeIdx];

    for( unsigned hIdx = 0; hIdx < maxSizeIdx; hIdx++ )
    {
      if( wIdx < 2 || hIdx < 2)
      {
        m_pTempCS[wIdx][hIdx] = nullptr;
        m_pBestCS[wIdx][hIdx] = nullptr;
        m_pTempCS2[wIdx][hIdx] = nullptr;
        m_pBestCS2[wIdx][hIdx] = nullptr;
        m_pOrgBuffer[wIdx][hIdx] = nullptr;
        m_pRspBuffer[wIdx][hIdx] = nullptr;
        continue;
      }

      Area area = Area( 0, 0, 1<<wIdx, 1<<hIdx );

      m_pTempCS[wIdx][hIdx] = new CodingStructure( m_unitCache, nullptr );
      m_pBestCS[wIdx][hIdx] = new CodingStructure( m_unitCache, nullptr );

      m_pTempCS[wIdx][hIdx]->create( chromaFormat, area, false );
      m_pBestCS[wIdx][hIdx]->create( chromaFormat, area, false );

      m_pTempCS2[wIdx][hIdx] = new CodingStructure( m_unitCache, nullptr );
      m_pBestCS2[wIdx][hIdx] = new CodingStructure( m_unitCache, nullptr );

      m_pTempCS2[wIdx][hIdx]->create( chromaFormat, area, false );
      m_pBestCS2[wIdx][hIdx]->create( chromaFormat, area, false );

      m_pOrgBuffer[wIdx][hIdx] = new PelStorage();
      m_pOrgBuffer[wIdx][hIdx]->create( chromaFormat, area );

      m_pRspBuffer[wIdx][hIdx] = new PelStorage();
      m_pRspBuffer[wIdx][hIdx]->create( CHROMA_400, area );
    }
  }

  m_cuChromaQpOffsetIdxPlus1 = 0;
  m_tempQpDiff = 0;
  m_globalCtuQpVector = globalCtuQpVector;

  m_SortedPelUnitBufs.create( chromaFormat, uiMaxSize, uiMaxSize );

  for( uint8_t i = 0; i < MAX_TMP_BUFS; i++)
  {
    m_aTmpStorageLCU[i].create(chromaFormat, Area(0, 0, uiMaxSize, uiMaxSize));
  }

  const unsigned maxDepth = 2*maxSizeIdx;
  m_CtxBuffer.resize( maxDepth );
  m_CurrCtx = 0;
  m_dbBuffer.create( chromaFormat, Area( 0, 0, encCfg.m_SourceWidth, encCfg.m_SourceHeight ) );
}


void EncCu::destroy()
{
  unsigned      maxSizeIdx  = MAX_CU_SIZE_IDX;
  for( unsigned wIdx = 0; wIdx < maxSizeIdx; wIdx++ )
  {
    for( unsigned hIdx = 0; hIdx < maxSizeIdx; hIdx++ )
    {
      if( m_pBestCS[wIdx][hIdx] ) m_pBestCS[wIdx][hIdx]->destroy();
      if( m_pTempCS[wIdx][hIdx] ) m_pTempCS[wIdx][hIdx]->destroy();

      delete m_pBestCS[wIdx][hIdx];
      delete m_pTempCS[wIdx][hIdx];

      if( m_pBestCS2[wIdx][hIdx] ) m_pBestCS2[wIdx][hIdx]->destroy();
      if( m_pTempCS2[wIdx][hIdx] ) m_pTempCS2[wIdx][hIdx]->destroy();

      delete m_pBestCS2[wIdx][hIdx];
      delete m_pTempCS2[wIdx][hIdx];

      if( m_pOrgBuffer[wIdx][hIdx] )m_pOrgBuffer[wIdx][hIdx]->destroy();
      delete m_pOrgBuffer[wIdx][hIdx];

      if( m_pRspBuffer[wIdx][hIdx] )m_pRspBuffer[wIdx][hIdx]->destroy();
      delete m_pRspBuffer[wIdx][hIdx];
    }

    delete[] m_pTempCS[wIdx];
    delete[] m_pBestCS[wIdx];
    delete[] m_pTempCS2[wIdx];
    delete[] m_pBestCS2[wIdx];

    delete[] m_pOrgBuffer[wIdx];
    delete[] m_pRspBuffer[wIdx];
  }

  delete[] m_pBestCS; m_pBestCS = nullptr;
  delete[] m_pTempCS; m_pTempCS = nullptr;
  delete[] m_pBestCS2; m_pBestCS2 = nullptr;
  delete[] m_pTempCS2; m_pTempCS2 = nullptr;

  delete[] m_pOrgBuffer; m_pOrgBuffer = nullptr;
  delete[] m_pRspBuffer; m_pRspBuffer= nullptr;

  m_SortedPelUnitBufs.destroy();

  for( uint8_t i = 0; i < MAX_TMP_BUFS; i++)
  {
    m_aTmpStorageLCU[i].destroy();
  }
  m_dbBuffer.destroy();
}


EncCu::~EncCu()
{
  destroy();
}

// ====================================================================================================================
// Public member functions
// ====================================================================================================================

void EncCu::encodeCtu( Picture* pic, int (&prevQP)[MAX_NUM_CH], uint32_t ctuXPosInCtus, uint32_t ctuYPosInCtus )
{
  CodingStructure&     cs          = *pic->cs;
  Slice*               slice       = cs.slice;
  const PreCalcValues& pcv         = *cs.pcv;
  const uint32_t       widthInCtus = pcv.widthInCtus;

  const int ctuRsAddr                 = ctuYPosInCtus * pcv.widthInCtus + ctuXPosInCtus;
  const uint32_t firstCtuRsAddrOfTile = 0;
  const uint32_t tileXPosInCtus       = firstCtuRsAddrOfTile % widthInCtus;

  const Position pos (ctuXPosInCtus * pcv.maxCUSize, ctuYPosInCtus * pcv.maxCUSize);
  const UnitArea ctuArea( cs.area.chromaFormat, Area( pos.x, pos.y, pcv.maxCUSize, pcv.maxCUSize ) );
  DTRACE_UPDATE( g_trace_ctx, std::make_pair( "ctu", ctuRsAddr ) );

  if ((cs.slice->sliceType != I_SLICE || cs.sps->IBC) && ctuXPosInCtus == tileXPosInCtus)
  {
    cs.motionLut.lut.resize(0);
    cs.motionLutBuf[ctuYPosInCtus].lut.resize(0);
  }

  if( m_pcEncCfg->m_ensureWppBitEqual && ctuXPosInCtus == 0 )
  {
    if( m_pcEncCfg->m_ensureWppBitEqual
        && m_pcEncCfg->m_numWppThreads < 1
        && ctuYPosInCtus > 0 )
    {
      m_cInterSearch.m_AffineProfList->resetAffineMVList ();
      m_cInterSearch.m_BlkUniMvInfoBuffer->resetUniMvList();
      m_CABACEstimator->initCtxModels( *slice );
    }

    if( m_pcEncCfg->m_entropyCodingSyncEnabled && ctuYPosInCtus )
    {
      m_CABACEstimator->getCtx() = m_syncPicCtx[ctuYPosInCtus-1];
    }

    prevQP[CH_L] = prevQP[CH_C] = slice->sliceQp; // hlm: call CU::predictQP() here!
  }
  else if (ctuRsAddr == firstCtuRsAddrOfTile )
  {
    m_CABACEstimator->initCtxModels( *slice );
    prevQP[CH_L] = prevQP[CH_C] = slice->sliceQp; // hlm: call CU::predictQP() here!
  }
  else if (ctuXPosInCtus == tileXPosInCtus && m_pcEncCfg->m_entropyCodingSyncEnabled)
  {
    // reset and then update contexts to the state at the end of the top-right CTU (if within current slice and tile).
    m_CABACEstimator->initCtxModels( *slice );

    if( cs.getCURestricted( pos.offset(0, -1), pos, slice->independentSliceIdx, 0, CH_L, TREE_D ) )
    {
      // Top-right is available, we use it.
      m_CABACEstimator->getCtx() = m_syncPicCtx[ctuYPosInCtus-1];
    }
    prevQP[CH_L] = prevQP[CH_C] = slice->sliceQp; // hlm: call CU::predictQP() here!
  }

  const double oldLambda = m_cRdCost.getLambda();
  xSetCtuQPRC( cs, slice, pic, ctuRsAddr );

  {
    xCompressCtu( cs, ctuArea, ctuRsAddr, prevQP );
  }

  m_CABACEstimator->resetBits();
  m_CABACEstimator->coding_tree_unit( cs, ctuArea, prevQP, ctuRsAddr, true, true );

  // Store probabilities of second CTU in line into buffer - used only if wavefront-parallel-processing is enabled.
  if( ctuXPosInCtus == tileXPosInCtus && m_pcEncCfg->m_entropyCodingSyncEnabled )
  {
    m_syncPicCtx[ctuYPosInCtus] = m_CABACEstimator->getCtx();
  }

  const int numberOfWrittenBits = int( m_CABACEstimator->getEstFracBits() >> SCALE_BITS );
  xUpdateAfterCtuRC( cs, slice, ctuArea, oldLambda, numberOfWrittenBits, ctuRsAddr );
}

// ====================================================================================================================
// Protected member functions
// ====================================================================================================================

void EncCu::xCompressCtu( CodingStructure& cs, const UnitArea& area, const unsigned ctuRsAddr, const int prevQP[] )
{
  m_modeCtrl.initCTUEncoding( *cs.slice );

  // init the partitioning manager
  Partitioner *partitioner = PartitionerFactory::get( *cs.slice );
  partitioner->initCtu( area, CH_L, *cs.slice );

  // init current context pointer
  m_CurrCtx = m_CtxBuffer.data();

  PelStorage* orgBuffer =   m_pOrgBuffer[Log2(area.lumaSize().width )][Log2(area.lumaSize().height )];
  PelStorage* rspBuffer =   m_pRspBuffer[Log2(area.lumaSize().width )][Log2(area.lumaSize().height )];
  CodingStructure *tempCS = m_pTempCS   [Log2(area.lumaSize().width )][Log2(area.lumaSize().height )];
  CodingStructure *bestCS = m_pBestCS   [Log2(area.lumaSize().width )][Log2(area.lumaSize().height )];
  cs.initSubStructure( *tempCS, partitioner->chType, partitioner->currArea(), false, orgBuffer, rspBuffer );
  cs.initSubStructure( *bestCS, partitioner->chType, partitioner->currArea(), false, orgBuffer, rspBuffer );
  m_CABACEstimator->determineNeighborCus( *tempCS, partitioner->currArea(), partitioner->chType, partitioner->treeType );

  // copy the relevant area
  UnitArea clippedArea = clipArea( partitioner->currArea(), cs.area );
  CPelUnitBuf org = cs.picture->getFilteredOrigBuffer().valid() ? cs.picture->getRspOrigBuf( clippedArea ) : cs.picture->getOrigBuf( clippedArea );
  tempCS->getOrgBuf( clippedArea ).copyFrom( org );
  const ReshapeData& reshapeData = cs.picture->reshapeData;
  if( cs.slice->lmcsEnabled && reshapeData.getCTUFlag() )
  {
    tempCS->getRspOrgBuf( clippedArea.Y() ).rspSignal( org.get( COMP_Y) , reshapeData.getFwdLUT() );
  }

  tempCS->currQP[CH_L] = bestCS->currQP[CH_L] =
  tempCS->baseQP       = bestCS->baseQP       = cs.slice->sliceQp;
  tempCS->prevQP[CH_L] = bestCS->prevQP[CH_L] = prevQP[CH_L];

  xCompressCU( tempCS, bestCS, *partitioner );
  // all signals were already copied during compression if the CTU was split - at this point only the structures are copied to the top level CS

  if ( m_wppMutex ) m_wppMutex->lock();

  cs.useSubStructure( *bestCS, partitioner->chType, TREE_D, CS::getArea( *bestCS, area, partitioner->chType, partitioner->treeType ), false );

  if ( m_wppMutex ) m_wppMutex->unlock();

  if (CS::isDualITree (cs) && isChromaEnabled (cs.pcv->chrFormat))
  {
    m_CABACEstimator->getCtx() = m_CurrCtx->start;

    partitioner->initCtu( area, CH_C, *cs.slice );

    cs.initSubStructure( *tempCS, partitioner->chType, partitioner->currArea(), false, orgBuffer, rspBuffer );
    cs.initSubStructure( *bestCS, partitioner->chType, partitioner->currArea(), false, orgBuffer, rspBuffer );
    m_CABACEstimator->determineNeighborCus( *tempCS, partitioner->currArea(), partitioner->chType, partitioner->treeType );
    tempCS->currQP[CH_C] = bestCS->currQP[CH_C] =
    tempCS->baseQP       = bestCS->baseQP       = cs.slice->sliceQp;
    tempCS->prevQP[CH_C] = bestCS->prevQP[CH_C] = prevQP[CH_C];

    xCompressCU( tempCS, bestCS, *partitioner );

    if ( m_wppMutex ) m_wppMutex->lock();

    cs.useSubStructure( *bestCS, partitioner->chType, TREE_D, CS::getArea( *bestCS, area, partitioner->chType, partitioner->treeType ), false );

    if ( m_wppMutex ) m_wppMutex->unlock();
  }

  if ( m_pcEncCfg->m_RCRateControlMode )
  {
    m_pcRateCtrl->encRCPic->lcu[ ctuRsAddr ].actualMSE = (double)bestCS->dist / (double)m_pcRateCtrl->encRCPic->lcu[ ctuRsAddr ].numberOfPixel;
  }

  // reset context states and uninit context pointer
  m_CABACEstimator->getCtx() = m_CurrCtx->start;
  m_CurrCtx                  = 0;
  delete partitioner;

  // Ensure that a coding was found
  // Selected mode's RD-cost must be not MAX_DOUBLE.
  CHECK( bestCS->cus.empty()                                   , "No possible encoding found" );
  CHECK( bestCS->cus[0]->predMode == NUMBER_OF_PREDICTION_MODES, "No possible encoding found" );
  CHECK( bestCS->cost             == MAX_DOUBLE                , "No possible encoding found" );
}



bool EncCu::xCheckBestMode( CodingStructure *&tempCS, CodingStructure *&bestCS, Partitioner &partitioner, const EncTestMode& encTestMode, const bool useEDO )
{
  bool bestCSUpdated = false;

  if( !tempCS->cus.empty() )
  {
    if( tempCS->cus.size() == 1 )
    {
      const CodingUnit& cu = *tempCS->cus.front();
      CHECK( cu.skip && !cu.pu->mergeFlag, "Skip flag without a merge flag is not allowed!" );
    }

    DTRACE_BEST_MODE( tempCS, bestCS, m_cRdCost.getLambda(true), useEDO );

    if( m_modeCtrl.useModeResult( encTestMode, tempCS, partitioner, useEDO ) )
    {
      std::swap( tempCS, bestCS );
      // store temp best CI for next CU coding
      m_CurrCtx->best = m_CABACEstimator->getCtx();
      bestCSUpdated = true;
    }
  }

  // reset context states
  m_CABACEstimator->getCtx() = m_CurrCtx->start;
  return bestCSUpdated;

}

void EncCu::xCompressCU( CodingStructure*& tempCS, CodingStructure*& bestCS, Partitioner& partitioner )
{
  PROFILER_SCOPE_AND_STAGE_EXT( 1, g_timeProfiler, P_COMPRESS_CU, tempCS, partitioner.chType );
  const Area& lumaArea = tempCS->area.Y();

  Slice&   slice      = *tempCS->slice;
  const PPS &pps      = *tempCS->pps;
  const SPS &sps      = *tempCS->sps;
  const uint32_t uiLPelX  = tempCS->area.Y().lumaPos().x;
  const uint32_t uiTPelY  = tempCS->area.Y().lumaPos().y;

  const UnitArea currCsArea = clipArea (CS::getArea (*bestCS, bestCS->area, partitioner.chType, partitioner.treeType), *bestCS->picture);

  if (m_pcEncCfg->m_usePerceptQPA && pps.useDQP && isLuma (partitioner.chType) && partitioner.currQgEnable() && m_pcEncCfg->m_RCRateControlMode != 1)
  {
    const PreCalcValues &pcv = *pps.pcv;
    Picture* const pic = bestCS->picture;
    const uint32_t ctuRsAddr = getCtuAddr (partitioner.currQgPos, pcv);

    if (partitioner.currSubdiv == 0) // CTU-level QP adaptation
    {
      if ( m_pcEncCfg->m_RCRateControlMode > 1 || m_pcEncCfg->m_usePerceptQPATempFiltISlice )
      {
        if ( m_pcEncCfg->m_RCRateControlMode > 1 && !( m_pcEncCfg->m_RCRateControlMode == 3 && pic->gopId > 0 ) )
        {
          // frame-level or GOP-level RC + QPA
          pic->ctuAdaptedQP[ ctuRsAddr ] += m_pcRateCtrl->encRCPic->picQPOffsetQPA;
          pic->ctuAdaptedQP[ ctuRsAddr ] = Clip3( 0, MAX_QP, (int)pic->ctuAdaptedQP[ ctuRsAddr ] );
          pic->ctuQpaLambda[ ctuRsAddr ] *= m_pcRateCtrl->encRCPic->picLambdaOffsetQPA;
          pic->ctuQpaLambda[ ctuRsAddr ] = Clip3( m_pcRateCtrl->encRCGOP->minEstLambda, m_pcRateCtrl->encRCGOP->maxEstLambda, pic->ctuQpaLambda[ ctuRsAddr ] );
        }
        m_tempQpDiff = pic->ctuAdaptedQP[ctuRsAddr] - BitAllocation::applyQPAdaptationSubCtu (&slice, m_pcEncCfg, lumaArea, m_pcEncCfg->m_usePerceptQPA > 2);
      }

      if ((!slice.isIntra() || slice.sps->IBC) && // Museum fix
          (uiLPelX + (pcv.maxCUSize >> 1) < m_pcEncCfg->m_SourceWidth) &&
          (uiTPelY + (pcv.maxCUSize >> 1) < m_pcEncCfg->m_SourceHeight))
      {
        const uint32_t h = lumaArea.height >> 1;
        const uint32_t w = lumaArea.width  >> 1;
        const int adQPTL = BitAllocation::applyQPAdaptationSubCtu (&slice, m_pcEncCfg, Area (uiLPelX + 0, uiTPelY + 0, w, h), m_pcEncCfg->m_usePerceptQPA > 2);
        const int adQPTR = BitAllocation::applyQPAdaptationSubCtu (&slice, m_pcEncCfg, Area (uiLPelX + w, uiTPelY + 0, w, h), m_pcEncCfg->m_usePerceptQPA > 2);
        const int adQPBL = BitAllocation::applyQPAdaptationSubCtu (&slice, m_pcEncCfg, Area (uiLPelX + 0, uiTPelY + h, w, h), m_pcEncCfg->m_usePerceptQPA > 2);
        const int adQPBR = BitAllocation::applyQPAdaptationSubCtu (&slice, m_pcEncCfg, Area (uiLPelX + w, uiTPelY + h, w, h), m_pcEncCfg->m_usePerceptQPA > 2);

        tempCS->currQP[partitioner.chType] = tempCS->baseQP =
        bestCS->currQP[partitioner.chType] = bestCS->baseQP = std::min (std::min (adQPTL, adQPTR), std::min (adQPBL, adQPBR));
        if ( ( m_pcEncCfg->m_RCRateControlMode > 1 && !( m_pcEncCfg->m_RCRateControlMode == 3 && pic->gopId > 0 ) ) || m_pcEncCfg->m_usePerceptQPATempFiltISlice )
        {
          if (m_pcEncCfg->m_usePerceptQPATempFiltISlice && (m_globalCtuQpVector->size() > ctuRsAddr) && (slice.TLayer == 0) // last CTU row of non-Intra key-frame
              && (m_pcEncCfg->m_IntraPeriod == 2 * m_pcEncCfg->m_GOPSize) && (ctuRsAddr >= pcv.widthInCtus) && (uiTPelY + pcv.maxCUSize > m_pcEncCfg->m_SourceHeight))
          {
            m_globalCtuQpVector->at (ctuRsAddr) = m_globalCtuQpVector->at (ctuRsAddr - pcv.widthInCtus);  // copy pumping reducing QP offset from top CTU neighbor
            tempCS->currQP[partitioner.chType] = tempCS->baseQP =
            bestCS->currQP[partitioner.chType] = bestCS->baseQP = tempCS->baseQP - m_globalCtuQpVector->at (ctuRsAddr);
          }
          tempCS->currQP[partitioner.chType] = tempCS->baseQP =
          bestCS->currQP[partitioner.chType] = bestCS->baseQP = Clip3 (0, MAX_QP, tempCS->baseQP + m_tempQpDiff);
        }
      }
      else
      {
        tempCS->currQP[partitioner.chType] = tempCS->baseQP =
        bestCS->currQP[partitioner.chType] = bestCS->baseQP = pic->ctuAdaptedQP[ctuRsAddr];
      }
      setUpLambda ( slice, pic->ctuQpaLambda[ctuRsAddr], pic->ctuAdaptedQP[ctuRsAddr], false, m_pcEncCfg->m_usePerceptQPA <= 4);
    }
    else if (slice.isIntra() && !slice.sps->IBC) // sub-CTU QPA
    {
      CHECK ((partitioner.currArea().lwidth() >= pcv.maxCUSize) || (partitioner.currArea().lheight() >= pcv.maxCUSize), "sub-CTU delta-QP error");
      tempCS->currQP[partitioner.chType] = tempCS->baseQP =
        BitAllocation::applyQPAdaptationSubCtu (&slice, m_pcEncCfg, lumaArea, m_pcEncCfg->m_usePerceptQPA > 2);
      if ( ( m_pcEncCfg->m_RCRateControlMode > 1 && !( m_pcEncCfg->m_RCRateControlMode == 3 && pic->gopId > 0 ) ) || m_pcEncCfg->m_usePerceptQPATempFiltISlice )
      {
        tempCS->currQP[partitioner.chType] = tempCS->baseQP = Clip3 (0, MAX_QP, tempCS->baseQP + m_tempQpDiff);
      }

      updateLambda (slice, pic->ctuQpaLambda[ctuRsAddr], pic->ctuAdaptedQP[ctuRsAddr], tempCS->baseQP, m_pcEncCfg->m_usePerceptQPA <= 4);
    }
  }

  m_modeCtrl.initCULevel( partitioner, *tempCS );
  if( partitioner.currQtDepth == 0 && partitioner.currMtDepth == 0 && !tempCS->slice->isIntra() && ( sps.SBT || sps.MTSInter ) )
  {
    int maxSLSize = sps.SBT ? (1 << tempCS->slice->sps->log2MaxTbSize) : MTS_INTER_MAX_CU_SIZE;
    m_modeCtrl.resetSaveloadSbt( maxSLSize );
  }
  m_sbtCostSave[0] = m_sbtCostSave[1] = MAX_DOUBLE;

  m_CurrCtx->start = m_CABACEstimator->getCtx();

  m_cuChromaQpOffsetIdxPlus1 = 0;

  if( slice.chromaQpAdjEnabled )
  {
    // TODO M0133 : double check encoder decisions with respect to chroma QG detection and actual encode
    int lgMinCuSize = sps.log2MinCodingBlockSize +
      std::max<int>(0, floorLog2(sps.CTUSize - sps.log2MinCodingBlockSize - int((slice.isIntra() ? slice.picHeader->cuChromaQpOffsetSubdivIntra : slice.picHeader->cuChromaQpOffsetSubdivInter) / 2)));
    if( partitioner.currQgChromaEnable() )
    {
      m_cuChromaQpOffsetIdxPlus1 = ( ( uiLPelX >> lgMinCuSize ) + ( uiTPelY >> lgMinCuSize ) ) % ( pps.chromaQpOffsetListLen + 1 );
    }
  }

  DTRACE_UPDATE( g_trace_ctx, std::make_pair( "cux", uiLPelX ) );
  DTRACE_UPDATE( g_trace_ctx, std::make_pair( "cuy", uiTPelY ) );
  DTRACE_UPDATE( g_trace_ctx, std::make_pair( "cuw", tempCS->area.lwidth() ) );
  DTRACE_UPDATE( g_trace_ctx, std::make_pair( "cuh", tempCS->area.lheight() ) );
  DTRACE( g_trace_ctx, D_COMMON, "@(%4d,%4d) [%2dx%2d]\n", tempCS->area.lx(), tempCS->area.ly(), tempCS->area.lwidth(), tempCS->area.lheight() );

  m_cInterSearch.resetSavedAffineMotion();
  bool Do_Affine = (m_pcEncCfg->m_Affine > 1) && (bestCS->slice->TLayer > 3) && (!m_pcEncCfg->m_SbTMVP) ? false : true;
  {
    const ComprCUCtx &cuECtx      = *m_modeCtrl.comprCUCtx;
    const CodingStructure& cs     = *tempCS;
    const PartSplit implicitSplit = partitioner.getImplicitSplit( cs );
    const bool isBoundary         = implicitSplit != CU_DONT_SPLIT;
    const bool lossless           = false;
    int qp                        = m_pcEncCfg->m_RCRateControlMode ? m_pcRateCtrl->rcQP : cs.baseQP;

    if( ! isBoundary )
    {
      if (pps.useDQP && partitioner.isSepTree (*tempCS) && isChroma (partitioner.chType))
      {
        const ChromaFormat chromaFm = tempCS->area.chromaFormat;
        const Position chromaCentral (tempCS->area.Cb().chromaPos().offset (tempCS->area.Cb().chromaSize().width >> 1, tempCS->area.Cb().chromaSize().height >> 1));
        const Position lumaRefPos (chromaCentral.x << getChannelTypeScaleX (CH_C, chromaFm), chromaCentral.y << getChannelTypeScaleY (CH_C, chromaFm));
        const CodingUnit* colLumaCu = bestCS->refCS->getCU (lumaRefPos, CH_L, TREE_D);
        // update qp
        qp = colLumaCu->qp;
      }

      m_cIntraSearch.reset();

      bool isReuseCU = m_modeCtrl.isReusingCuValid( cs, partitioner, qp );
      if( isReuseCU )
      {
        xReuseCachedResult( tempCS, bestCS, partitioner );
      }
      else
      {
        // add first pass modes
        if ( !slice.isIRAP() && !( cs.area.lwidth() == 4 && cs.area.lheight() == 4 ) && !partitioner.isConsIntra() )
        {
          // add inter modes
          if( m_pcEncCfg->m_useEarlySkipDetection )
          {
            EncTestMode encTestMode = {ETM_INTER_ME, ETO_STANDARD, qp, lossless};
            if( m_modeCtrl.tryMode( encTestMode, cs, partitioner ) )
            {
              xCheckRDCostInter( tempCS, bestCS, partitioner, encTestMode );
            }
            if (m_pcEncCfg->m_Geo && cs.slice->isInterB())
            {
              EncTestMode encTestModeGeo = { ETM_MERGE_GEO, ETO_STANDARD, qp, lossless };
              if (m_modeCtrl.tryMode(encTestModeGeo, cs, partitioner))
              {
                xCheckRDCostMergeGeo(tempCS, bestCS, partitioner, encTestModeGeo);
              }
            }
            if (m_pcEncCfg->m_Affine || cs.sps->SbtMvp)
            {
              EncTestMode encTestModeAffine = { ETM_AFFINE, ETO_STANDARD, qp, lossless };
              if (m_modeCtrl.tryMode(encTestModeAffine, cs, partitioner) && Do_Affine)
              {
                xCheckRDCostAffineMerge(tempCS, bestCS, partitioner, encTestModeAffine);
              }
            }

            EncTestMode encTestModeSkip = {ETM_MERGE_SKIP, ETO_STANDARD, qp, lossless};
            if( m_modeCtrl.tryMode( encTestModeSkip, cs, partitioner ) )
            {
              xCheckRDCostMerge( tempCS, bestCS, partitioner, encTestModeSkip );
            }
          }
          else
          {
            if ((m_pcEncCfg->m_Affine == 1) || (cs.sps->SbtMvp && m_pcEncCfg->m_Affine == 0))
            {
              EncTestMode encTestModeAffine = { ETM_AFFINE, ETO_STANDARD, qp, lossless };
              if (m_modeCtrl.tryMode(encTestModeAffine, cs, partitioner) && Do_Affine)
              {
                xCheckRDCostAffineMerge(tempCS, bestCS, partitioner, encTestModeAffine);
              }
            }
            EncTestMode encTestModeSkip = {ETM_MERGE_SKIP, ETO_STANDARD, qp, lossless};
            if( m_modeCtrl.tryMode( encTestModeSkip, cs, partitioner ) )
            {
              xCheckRDCostMerge( tempCS, bestCS, partitioner, encTestModeSkip );

              CodingUnit* cu = bestCS->getCU(partitioner.chType, partitioner.treeType);
              if (cu)
              cu->mmvdSkip = cu->skip == false ? false : cu->mmvdSkip;
            }

            if (m_pcEncCfg->m_Affine > 1)
            {
              EncTestMode encTestModeAffine = { ETM_AFFINE, ETO_STANDARD, qp, lossless };
              if (m_modeCtrl.tryMode(encTestModeAffine, cs, partitioner) && Do_Affine)
              {
                xCheckRDCostAffineMerge(tempCS, bestCS, partitioner, encTestModeAffine);
              }
            }
            if (m_pcEncCfg->m_Geo && cs.slice->isInterB())
            {
              EncTestMode encTestModeGeo = { ETM_MERGE_GEO, ETO_STANDARD, qp, lossless };
              if (m_modeCtrl.tryMode(encTestModeGeo, cs, partitioner))
              {
                xCheckRDCostMergeGeo(tempCS, bestCS, partitioner, encTestModeGeo);
              }
            }
            EncTestMode encTestMode = {ETM_INTER_ME, ETO_STANDARD, qp, lossless};
            if( m_modeCtrl.tryMode( encTestMode, cs, partitioner ) )
            {
              xCheckRDCostInter( tempCS, bestCS, partitioner, encTestMode );
            }
          }
          if (m_pcEncCfg->m_AMVRspeed)
          {
            double bestIntPelCost = MAX_DOUBLE;

            EncTestMode encTestMode = {ETM_INTER_IMV, ETO_STANDARD, qp, lossless};
            if( m_modeCtrl.tryMode( encTestMode, cs, partitioner ) )
            {
              const bool skipAltHpelIF = ( int( ( encTestMode.opts & ETO_IMV ) >> ETO_IMV_SHIFT ) == 4 ) && ( bestIntPelCost > 1.25 * bestCS->cost );
              if (!skipAltHpelIF)
              {
                tempCS->bestCS = bestCS;
                xCheckRDCostInterIMV(tempCS, bestCS, partitioner, encTestMode );
                tempCS->bestCS = nullptr;
              }
            }
          }
        }
        if( m_pcEncCfg->m_EDO && bestCS->cost != MAX_DOUBLE )
        {
          xCalDebCost(*bestCS, partitioner);
        }

        // add intra modes
        EncTestMode encTestMode( {ETM_INTRA, ETO_STANDARD, qp, lossless} );
        if( !partitioner.isConsInter() && m_modeCtrl.tryMode( encTestMode, cs, partitioner ) )
        {
          xCheckRDCostIntra( tempCS, bestCS, partitioner, encTestMode );
        }
      } // reusing cu

      m_modeCtrl.beforeSplit( partitioner );

      if (cuECtx.bestCS && ((cuECtx.bestCostNoImv == (MAX_DOUBLE * .5) || cuECtx.isReusingCu) && !slice.isIntra()) )
      {
        m_cInterSearch.loadGlobalUniMvs( lumaArea, *pps.pcv );
      }
    } //boundary

    //////////////////////////////////////////////////////////////////////////
    // split modes
    EncTestMode lastTestMode;

    if( cuECtx.qtBeforeBt )
    {
      EncTestMode encTestMode( { ETM_SPLIT_QT, ETO_STANDARD, qp, false } );
      if( m_modeCtrl.trySplit( encTestMode, cs, partitioner, lastTestMode ) )
      {
        lastTestMode = encTestMode;
        xCheckModeSplit( tempCS, bestCS, partitioner, encTestMode );
      }
    }

    if( partitioner.canSplit( CU_HORZ_SPLIT, cs ) )
    {
      // add split modes
      EncTestMode encTestMode( { ETM_SPLIT_BT_H, ETO_STANDARD, qp, false } );
      if( m_modeCtrl.trySplit( encTestMode, cs, partitioner, lastTestMode ) )
      {
        lastTestMode = encTestMode;
        xCheckModeSplit( tempCS, bestCS, partitioner, encTestMode );
      }
    }

    if( partitioner.canSplit( CU_VERT_SPLIT, cs ) )
    {
      // add split modes
      EncTestMode encTestMode( { ETM_SPLIT_BT_V, ETO_STANDARD, qp, false } );
      if( m_modeCtrl.trySplit( encTestMode, cs, partitioner, lastTestMode ) )
      {
        lastTestMode = encTestMode;
        xCheckModeSplit( tempCS, bestCS, partitioner, encTestMode );
      }
    }

    if( partitioner.canSplit( CU_TRIH_SPLIT, cs ) )
    {
      // add split modes
      EncTestMode encTestMode( { ETM_SPLIT_TT_H, ETO_STANDARD, qp, false } );
      if( m_modeCtrl.trySplit( encTestMode, cs, partitioner, lastTestMode ) )
      {
        lastTestMode = encTestMode;
        xCheckModeSplit( tempCS, bestCS, partitioner, encTestMode );
      }
    }

    if( partitioner.canSplit( CU_TRIV_SPLIT, cs ) )
    {
      // add split modes
      EncTestMode encTestMode( { ETM_SPLIT_TT_V, ETO_STANDARD, qp, false } );
      if( m_modeCtrl.trySplit( encTestMode, cs, partitioner, lastTestMode ) )
      {
        lastTestMode = encTestMode;
        xCheckModeSplit( tempCS, bestCS, partitioner, encTestMode );
      }
    }

    if( !cuECtx.qtBeforeBt )
    {
      EncTestMode encTestMode( { ETM_SPLIT_QT, ETO_STANDARD, qp, false } );
      if( m_modeCtrl.trySplit( encTestMode, cs, partitioner, lastTestMode ) )
      {
        lastTestMode = encTestMode;
        xCheckModeSplit( tempCS, bestCS, partitioner, encTestMode );
      }
    }
  }

  if( bestCS->cus.empty() )
  {
    m_modeCtrl.finishCULevel( partitioner );
    return;
  }

  //////////////////////////////////////////////////////////////////////////
  // Finishing CU
  // set context states
  m_CABACEstimator->getCtx() = m_CurrCtx->best;

  // QP from last processed CU for further processing
  //copy the qp of the last non-chroma CU
  int numCUInThisNode = (int)bestCS->cus.size();
  if( numCUInThisNode > 1 && bestCS->cus.back()->chType == CH_C && !CS::isDualITree( *bestCS ) )
  {
    CHECK( bestCS->cus[numCUInThisNode-2]->chType != CH_L, "wrong chType" );
    bestCS->prevQP[partitioner.chType] = bestCS->cus[numCUInThisNode-2]->qp;
  }
  else
  {
  bestCS->prevQP[partitioner.chType] = bestCS->cus.back()->qp;
  }
  if ((!slice.isIntra() || slice.sps->IBC)
    && partitioner.chType == CH_L
    && bestCS->cus.size() == 1 && (bestCS->cus.back()->predMode == MODE_INTER || bestCS->cus.back()->predMode == MODE_IBC)
    && bestCS->area.Y() == (*bestCS->cus.back()).Y()
    )
  {
    const CodingUnit&     cu = *bestCS->cus.front();
    const PredictionUnit& pu = *cu.pu;

    if (!cu.affine && !cu.geo)
    {
      const MotionInfo &mi = pu.getMotionInfo();
      HPMVInfo hMi( mi, (mi.interDir == 3) ? cu.BcwIdx : BCW_DEFAULT, cu.imv == IMV_HPEL );
      cu.cs->addMiToLut( /*CU::isIBC(cu) ? cu.cs->motionLut.lutIbc :*/ cu.cs->motionLut.lut, hMi );
    }
  }

  bestCS->picture->getRecoBuf( currCsArea ).copyFrom( bestCS->getRecoBuf( currCsArea ) );
  m_modeCtrl.finishCULevel( partitioner );
  if( m_cIntraSearch.getSaveCuCostInSCIPU() && bestCS->cus.size() == 1 )
  {
    m_cIntraSearch.saveCuAreaCostInSCIPU( Area( partitioner.currArea().lumaPos(), partitioner.currArea().lumaSize() ), bestCS->cost );
  }

  // Assert if Best prediction mode is NONE
  // Selected mode's RD-cost must be not MAX_DOUBLE.
  CHECK( bestCS->cus.empty()                                   , "No possible encoding found" );
  CHECK( bestCS->cus[0]->predMode == NUMBER_OF_PREDICTION_MODES, "No possible encoding found" );
  CHECK( bestCS->cost             == MAX_DOUBLE                , "No possible encoding found" );
}


void EncCu::xCheckModeSplit(CodingStructure *&tempCS, CodingStructure *&bestCS, Partitioner &partitioner, const EncTestMode& encTestMode )
{
  const ModeType modeTypeParent  = partitioner.modeType;
  const TreeType treeTypeParent  = partitioner.treeType;
  const ChannelType chTypeParent = partitioner.chType;

  int signalModeConsVal = tempCS->signalModeCons( getPartSplit( encTestMode ), partitioner, modeTypeParent );
  int numRoundRdo = signalModeConsVal == LDT_MODE_TYPE_SIGNAL ? 2 : 1;
  bool skipInterPass = false;
  for( int i = 0; i < numRoundRdo; i++ )
  {
    //change cons modes
    if( signalModeConsVal == LDT_MODE_TYPE_SIGNAL )
    {
      CHECK( numRoundRdo != 2, "numRoundRdo shall be 2 - [LDT_MODE_TYPE_SIGNAL]" );
      partitioner.modeType = (i == 0) ? MODE_TYPE_INTER : MODE_TYPE_INTRA;
    }
    else if( signalModeConsVal == LDT_MODE_TYPE_INFER )
    {
      CHECK( numRoundRdo != 1, "numRoundRdo shall be 1 - [LDT_MODE_TYPE_INFER]" );
      partitioner.modeType = MODE_TYPE_INTRA;
    }
    else if( signalModeConsVal == LDT_MODE_TYPE_INHERIT )
    {
      CHECK( numRoundRdo != 1, "numRoundRdo shall be 1 - [LDT_MODE_TYPE_INHERIT]" );
      partitioner.modeType = modeTypeParent;
    }

    //for lite intra encoding fast algorithm, set the status to save inter coding info
    if( modeTypeParent == MODE_TYPE_ALL && partitioner.modeType == MODE_TYPE_INTER )
    {
      m_cIntraSearch.setSaveCuCostInSCIPU( true );
      m_cIntraSearch.setNumCuInSCIPU( 0 );
    }
    else if( modeTypeParent == MODE_TYPE_ALL && partitioner.modeType != MODE_TYPE_INTER )
    {
      m_cIntraSearch.setSaveCuCostInSCIPU( false );
      if( partitioner.modeType == MODE_TYPE_ALL )
      {
        m_cIntraSearch.setNumCuInSCIPU( 0 );
      }
    }

    xCheckModeSplitInternal( tempCS, bestCS, partitioner, encTestMode, modeTypeParent, skipInterPass );
    //recover cons modes
    partitioner.modeType = modeTypeParent;
    partitioner.treeType = treeTypeParent;
    partitioner.chType = chTypeParent;
    if( modeTypeParent == MODE_TYPE_ALL )
    {
      m_cIntraSearch.setSaveCuCostInSCIPU( false );
      if( numRoundRdo == 2 && partitioner.modeType == MODE_TYPE_INTRA )
      {
        m_cIntraSearch.initCuAreaCostInSCIPU();
      }
    }
    if( skipInterPass )
    {
      break;
    }
  }
}

void EncCu::xCheckModeSplitInternal(CodingStructure *&tempCS, CodingStructure *&bestCS, Partitioner &partitioner, const EncTestMode& encTestMode, const ModeType modeTypeParent, bool& skipInterPass )
{
  const int qp                   = encTestMode.qp;
  const int oldPrevQp            = tempCS->prevQP[partitioner.chType];
  const auto oldMotionLut        = tempCS->motionLut;
  const ReshapeData& reshapeData = tempCS->picture->reshapeData;

  const PartSplit split = getPartSplit( encTestMode );
  const ModeType modeTypeChild = partitioner.modeType;

  CHECK( split == CU_DONT_SPLIT, "No proper split provided!" );

  tempCS->initStructData( qp );

  m_CABACEstimator->getCtx() = m_CurrCtx->start;

  const uint16_t split_ctx_size = Ctx::SplitFlag.size() + Ctx::SplitQtFlag.size() + Ctx::SplitHvFlag.size() + Ctx::Split12Flag.size() + Ctx::ModeConsFlag.size();
  const TempCtx  ctxSplitFlags( m_CtxCache, SubCtx(CtxSet(Ctx::SplitFlag(), split_ctx_size), m_CABACEstimator->getCtx()));

  m_CABACEstimator->resetBits();

  m_CABACEstimator->split_cu_mode( split, *tempCS, partitioner );
  m_CABACEstimator->mode_constraint( split, *tempCS, partitioner, modeTypeChild );

  const double factor = ( tempCS->currQP[partitioner.chType] > 30 ? 1.1 : 1.075 ) - ( m_pcEncCfg->m_qtbttSpeedUp ? 0.025 : 0.0 ) + ( ( m_pcEncCfg->m_qtbttSpeedUp && isChroma( partitioner.chType ) ) ? 0.2 : 0.0 );

  const double cost   = m_cRdCost.calcRdCost( uint64_t( m_CABACEstimator->getEstFracBits() + ( ( bestCS->fracBits ) / factor ) ), Distortion( bestCS->dist / factor ) ) + bestCS->costDbOffset / factor;

  m_CABACEstimator->getCtx() = SubCtx(CtxSet(Ctx::SplitFlag(), split_ctx_size), ctxSplitFlags);

  if (cost > bestCS->cost + bestCS->costDbOffset )
  {
//    DTRACE( g_trace_ctx, D_TMP, "%d exit split %f %f %f\n", g_trace_ctx->getChannelCounter(D_TMP), cost, bestCS->cost, bestCS->costDbOffset );
    xCheckBestMode( tempCS, bestCS, partitioner, encTestMode );
    return;
  }

  const bool chromaNotSplit = modeTypeParent == MODE_TYPE_ALL && modeTypeChild == MODE_TYPE_INTRA ? true : false;

  if( partitioner.treeType == TREE_D )
  {
    if( chromaNotSplit )
    {
      CHECK( partitioner.chType != CH_L, "chType must be luma" );
      partitioner.treeType = TREE_L;
    }
    else
    {
      partitioner.treeType = TREE_D;
    }
  }

  CHECK(!(split == CU_QUAD_SPLIT || split == CU_HORZ_SPLIT || split == CU_VERT_SPLIT
    || split == CU_TRIH_SPLIT || split == CU_TRIV_SPLIT), "invalid split type");

  partitioner.splitCurrArea( split, *tempCS );
  bool qgEnableChildren = partitioner.currQgEnable(); // QG possible at children level

  m_CurrCtx++;

  AffineMVInfo tmpMVInfo;
  bool isAffMVInfoSaved = m_cInterSearch.m_AffineProfList->savePrevAffMVInfo(0, tmpMVInfo );

  BlkUniMvInfo tmpUniMvInfo;
  bool         isUniMvInfoSaved = false;
  if (!tempCS->slice->isIntra())
  {
    m_cInterSearch.m_BlkUniMvInfoBuffer->savePrevUniMvInfo(tempCS->area.Y(), tmpUniMvInfo, isUniMvInfoSaved);
  }

  DeriveCtx deriveCtx = m_CABACEstimator->getDeriveCtx();

  do
  {
    const auto &subCUArea  = partitioner.currArea();

    if( tempCS->picture->Y().contains( subCUArea.lumaPos() ) )
    {
      const unsigned wIdx    = Log2(subCUArea.lwidth ());
      const unsigned hIdx    = Log2(subCUArea.lheight());

      PelStorage* orgBuffer =   m_pOrgBuffer[wIdx][hIdx];
      PelStorage* rspBuffer =   m_pRspBuffer[wIdx][hIdx];
      CodingStructure *tempSubCS = m_pTempCS[wIdx][hIdx];
      CodingStructure *bestSubCS = m_pBestCS[wIdx][hIdx];
      // copy org buffer
      orgBuffer->copyFrom( tempCS->getOrgBuf( subCUArea ) );
      if( tempCS->slice->lmcsEnabled && reshapeData.getCTUFlag() )
      {
        rspBuffer->Y().copyFrom( tempCS->getRspOrgBuf( subCUArea.Y() ) );
      }
      tempCS->initSubStructure( *tempSubCS, partitioner.chType, subCUArea, false, orgBuffer, rspBuffer );
      tempCS->initSubStructure( *bestSubCS, partitioner.chType, subCUArea, false, orgBuffer, rspBuffer );
      m_CABACEstimator->determineNeighborCus( *tempSubCS, partitioner.currArea(), partitioner.chType, partitioner.treeType );

      tempSubCS->bestParent = bestSubCS->bestParent = bestCS;

      xCompressCU(tempSubCS, bestSubCS, partitioner );

      tempSubCS->bestParent = bestSubCS->bestParent = nullptr;

      if( bestSubCS->cost == MAX_DOUBLE )
      {
        CHECK( split == CU_QUAD_SPLIT, "Split decision reusing cannot skip quad split" );
        tempCS->cost = MAX_DOUBLE;
        tempCS->costDbOffset = 0;
        m_CurrCtx--;
        partitioner.exitCurrSplit();
        xCheckBestMode( tempCS, bestCS, partitioner, encTestMode );
        if( partitioner.chType == CH_L )
        {
          tempCS->motionLut = oldMotionLut;
        }

        m_CABACEstimator->getDeriveCtx() = deriveCtx;
        return;
      }

      tempCS->useSubStructure( *bestSubCS, partitioner.chType, TREE_D, CS::getArea( *tempCS, subCUArea, partitioner.chType, partitioner.treeType ), true );

      if( partitioner.currQgEnable() )
      {
        tempCS->prevQP[partitioner.chType] = bestSubCS->prevQP[partitioner.chType];
      }
      if( partitioner.isConsInter() )
      {
        for( int i = 0; i < bestSubCS->cus.size(); i++ )
        {
          CHECK( bestSubCS->cus[i]->predMode != MODE_INTER, "all CUs must be inter mode in an Inter coding region (SCIPU)" );
        }
      }
      else if( partitioner.isConsIntra() )
      {
        for( int i = 0; i < bestSubCS->cus.size(); i++ )
        {
          CHECK( bestSubCS->cus[i]->predMode == MODE_INTER, "all CUs must not be inter mode in an Intra coding region (SCIPU)" );
        }
      }

      tempSubCS->releaseIntermediateData();
      bestSubCS->releaseIntermediateData();
      if( !tempCS->slice->isIntra() && partitioner.isConsIntra() )
      {
        tempCS->cost = m_cRdCost.calcRdCost( tempCS->fracBits, tempCS->dist );
        if( tempCS->cost > bestCS->cost )
        {
          tempCS->cost = MAX_DOUBLE;
          tempCS->costDbOffset = 0;
          m_CurrCtx--;
          partitioner.exitCurrSplit();
          if( partitioner.chType == CH_L )
          {
            tempCS->motionLut = oldMotionLut;
          }

          m_CABACEstimator->getDeriveCtx() = deriveCtx;
          return;
        }
      }
    }
  } while( partitioner.nextPart( *tempCS ) );

  partitioner.exitCurrSplit();

  m_CurrCtx--;

  m_CABACEstimator->getDeriveCtx() = deriveCtx;

  if( chromaNotSplit )
  {
    //Note: In local dual tree region, the chroma CU refers to the central luma CU's QP.
    //If the luma CU QP shall be predQP (no residual in it and before it in the QG), it must be revised to predQP before encoding the chroma CU
    //Otherwise, the chroma CU uses predQP+deltaQP in encoding but is decoded as using predQP, thus causing encoder-decoded mismatch on chroma qp.
    if( tempCS->pps->useDQP )
    {
      //find parent CS that including all coded CUs in the QG before this node
      CodingStructure* qgCS = tempCS;
      bool deltaQpCodedBeforeThisNode = false;
      if( partitioner.currArea().lumaPos() != partitioner.currQgPos )
      {
        int numParentNodeToQgCS = 0;
        while( qgCS->area.lumaPos() != partitioner.currQgPos )
        {
          CHECK( qgCS->parent == nullptr, "parent of qgCS shall exsit" );
          qgCS = qgCS->parent;
          numParentNodeToQgCS++;
        }

        //check whether deltaQP has been coded (in luma CU or luma&chroma CU) before this node
        CodingStructure* parentCS = tempCS->parent;
        for( int i = 0; i < numParentNodeToQgCS; i++ )
        {
          //checking each parent
          CHECK( parentCS == nullptr, "parentCS shall exsit" );
          for( const auto &cu : parentCS->cus )
          {
            if( cu->rootCbf && !isChroma( cu->chType ) )
            {
              deltaQpCodedBeforeThisNode = true;
              break;
            }
          }
          parentCS = parentCS->parent;
        }
      }

      //revise luma CU qp before the first luma CU with residual in the SCIPU to predQP
      if( !deltaQpCodedBeforeThisNode )
      {
        //get pred QP of the QG
        const CodingUnit* cuFirst = qgCS->getCU( CH_L, TREE_D );
        CHECK( cuFirst->lumaPos() != partitioner.currQgPos, "First cu of the Qg is wrong" );
        int predQp = CU::predictQP( *cuFirst, qgCS->prevQP[CH_L] );

        //revise to predQP
        int firstCuHasResidual = (int)tempCS->cus.size();
        for( int i = 0; i < tempCS->cus.size(); i++ )
        {
          if( tempCS->cus[i]->rootCbf )
          {
            firstCuHasResidual = i;
            break;
          }
        }

        for( int i = 0; i < firstCuHasResidual; i++ )
        {
          tempCS->cus[i]->qp = predQp;
        }
      }
    }

    partitioner.chType   = CH_C;
    partitioner.treeType = TREE_C;

    m_CurrCtx++;

    const unsigned wIdx = Log2(partitioner.currArea().lwidth() );
    const unsigned hIdx = Log2(partitioner.currArea().lheight());
    CodingStructure *tempCSChroma = m_pTempCS2[wIdx][hIdx];
    CodingStructure *bestCSChroma = m_pBestCS2[wIdx][hIdx];

    tempCS->initSubStructure( *tempCSChroma, partitioner.chType, partitioner.currArea(), false );
    tempCS->initSubStructure( *bestCSChroma, partitioner.chType, partitioner.currArea(), false );
    m_CABACEstimator->determineNeighborCus( *bestCSChroma, partitioner.currArea(), partitioner.chType, partitioner.treeType );
    tempCSChroma->refCS = tempCS;
    bestCSChroma->refCS = tempCS;
    xCompressCU( tempCSChroma, bestCSChroma, partitioner );

    //attach chromaCS to luma CS and update cost
    tempCS->useSubStructure( *bestCSChroma, partitioner.chType, TREE_D, CS::getArea( *bestCSChroma, partitioner.currArea(), partitioner.chType, partitioner.treeType ), true );

    //release tmp resource
    tempCSChroma->releaseIntermediateData();
    bestCSChroma->releaseIntermediateData();

    m_CurrCtx--;

    //recover luma tree status
    partitioner.chType = CH_L;
    partitioner.treeType = TREE_D;
    partitioner.modeType = MODE_TYPE_ALL;
  }

  // Finally, generate split-signaling bits for RD-cost check
  const PartSplit implicitSplit = partitioner.getImplicitSplit( *tempCS );

  {
    bool enforceQT = implicitSplit == CU_QUAD_SPLIT;

    // LARGE CTU bug
    if( m_pcEncCfg->m_useFastLCTU )
    {
      unsigned minDepth = m_modeCtrl.comprCUCtx->minDepth;
      if( minDepth > partitioner.currQtDepth )
      {
        // enforce QT
        enforceQT = true;
      }
    }

    if( !enforceQT )
    {
      m_CABACEstimator->resetBits();

      m_CABACEstimator->split_cu_mode( split, *tempCS, partitioner );
      partitioner.modeType = modeTypeParent;
      m_CABACEstimator->mode_constraint( split, *tempCS, partitioner, modeTypeChild );
      tempCS->fracBits += m_CABACEstimator->getEstFracBits(); // split bits
    }
  }

  tempCS->cost = m_cRdCost.calcRdCost( tempCS->fracBits, tempCS->dist );

  // Check Delta QP bits for splitted structure
  if( !qgEnableChildren ) // check at deepest QG level only
    xCheckDQP( *tempCS, partitioner, true );

  // If the configuration being tested exceeds the maximum number of bytes for a slice / slice-segment, then
  // a proper RD evaluation cannot be performed. Therefore, termination of the
  // slice/slice-segment must be made prior to this CTU.
  // This can be achieved by forcing the decision to be that of the rpcTempCU.
  // The exception is each slice / slice-segment must have at least one CTU.
  if (bestCS->cost == MAX_DOUBLE)
  {
    bestCS->costDbOffset = 0;
  }

  if( tempCS->cus.size() > 0 && modeTypeParent == MODE_TYPE_ALL && modeTypeChild == MODE_TYPE_INTER )
  {
    int areaSizeNoResiCu = 0;
    for( int k = 0; k < tempCS->cus.size(); k++ )
    {
      areaSizeNoResiCu += (tempCS->cus[k]->rootCbf == false) ? tempCS->cus[k]->lumaSize().area() : 0;
    }
    if( areaSizeNoResiCu >= (tempCS->area.lumaSize().area() >> 1) )
    {
      skipInterPass = true;
    }
  }

  // RD check for sub partitioned coding structure.
  xCheckBestMode( tempCS, bestCS, partitioner, encTestMode, m_pcEncCfg->m_EDO );

  if( isAffMVInfoSaved)
  {
    m_cInterSearch.m_AffineProfList->addAffMVInfo(tmpMVInfo);
  }

  if (!tempCS->slice->isIntra() && isUniMvInfoSaved)
  {
    m_cInterSearch.m_BlkUniMvInfoBuffer->addUniMvInfo(tmpUniMvInfo);
  }

  tempCS->motionLut = oldMotionLut;
  tempCS->releaseIntermediateData();
  tempCS->prevQP[partitioner.chType] = oldPrevQp;
}


void EncCu::xCheckRDCostIntra( CodingStructure *&tempCS, CodingStructure *&bestCS, Partitioner &partitioner, const EncTestMode& encTestMode )
{
  PROFILER_SCOPE_AND_STAGE_EXT( 1, g_timeProfiler, P_INTRA, tempCS, partitioner.chType );

  tempCS->initStructData( encTestMode.qp );

  CodingUnit &cu      = tempCS->addCU( CS::getArea( *tempCS, tempCS->area, partitioner.chType, partitioner.treeType ), partitioner.chType );

  partitioner.setCUData( cu );
  cu.slice            = tempCS->slice;
  cu.tileIdx          = 0;
  cu.skip             = false;
  cu.mmvdSkip         = false;
  cu.predMode         = MODE_INTRA;
  cu.chromaQpAdj      = m_cuChromaQpOffsetIdxPlus1;
  cu.qp               = encTestMode.qp;
  cu.ispMode          = NOT_INTRA_SUBPARTITIONS;

  CU::addPUs( cu );

  tempCS->interHad    = m_modeCtrl.comprCUCtx->interHad;

  if( isLuma( partitioner.chType ) )
  {
    if (!tempCS->slice->isIntra() && bestCS)
    {
      m_cIntraSearch.estIntraPredLumaQT(cu, partitioner, bestCS->cost);
    }
    else
    {
      m_cIntraSearch.estIntraPredLumaQT(cu, partitioner);
    }

    if( !partitioner.isSepTree( *tempCS ) )
    {
      tempCS->lumaCost = m_cRdCost.calcRdCost( tempCS->fracBits, tempCS->dist );
    }
    if (m_pcEncCfg->m_usePbIntraFast && tempCS->dist == MAX_DISTORTION && tempCS->interHad == 0)
    {
      // JEM assumes only perfect reconstructions can from now on beat the inter mode
      m_modeCtrl.comprCUCtx->interHad = 0;
      return;// th new continue;
    }
  }

  if( tempCS->area.chromaFormat != CHROMA_400 && ( partitioner.chType == CH_C || !cu.isSepTree() ) )
  {
    m_cIntraSearch.estIntraPredChromaQT( cu, partitioner );
  }

  cu.rootCbf = false;

  for( uint32_t t = 0; t < getNumberValidTBlocks( *cu.cs->pcv ); t++ )
  {
    cu.rootCbf |= cu.firstTU->cbf[t] != 0;
  }

  // Get total bits for current mode: encode CU
  m_CABACEstimator->resetBits();

  if( (!cu.cs->slice->isIntra() || cu.cs->slice->sps->IBC) && cu.Y().valid())
  {
    m_CABACEstimator->cu_skip_flag ( cu );
  }
  m_CABACEstimator->pred_mode      ( cu );
  m_CABACEstimator->cu_pred_data   ( cu );
  m_CABACEstimator->bdpcm_mode     ( cu, ComponentID(partitioner.chType) );

  // Encode Coefficients
  CUCtx cuCtx;
  cuCtx.isDQPCoded = true;
  cuCtx.isChromaQpAdjCoded = true;
  m_CABACEstimator->cu_residual( cu, partitioner, cuCtx );

  tempCS->fracBits = m_CABACEstimator->getEstFracBits();
  tempCS->cost     = m_cRdCost.calcRdCost(tempCS->fracBits, tempCS->dist);

  xEncodeDontSplit( *tempCS, partitioner );

  xCheckDQP( *tempCS, partitioner );

  if( m_pcEncCfg->m_EDO )
  {
    xCalDebCost( *tempCS, partitioner );
  }

  DTRACE_MODE_COST( *tempCS, m_cRdCost.getLambda(true) );
  xCheckBestMode( tempCS, bestCS, partitioner, encTestMode, m_pcEncCfg->m_EDO );

  STAT_COUNT_CU_MODES( partitioner.chType == CH_L, g_cuCounters1D[CU_MODES_TESTED][0][!tempCS->slice->isIntra() + tempCS->slice->depth] );
  STAT_COUNT_CU_MODES( partitioner.chType == CH_L && !tempCS->slice->isIntra(), g_cuCounters2D[CU_MODES_TESTED][Log2( tempCS->area.lheight() )][Log2( tempCS->area.lwidth() )] );
}

void EncCu::xSetCtuQPRC( CodingStructure& cs, const Slice* slice, const Picture* pic, const int ctuRsAddr )
{
  if ( m_pcEncCfg->m_RCRateControlMode < 1 )
  {
    return;
  }

  int estQP = slice->sliceQp;
  double estLambda = -1.0;
  double bpp = -1.0;

  if ( ( pic->slices[ 0 ]->isIRAP() && m_pcEncCfg->m_RCForceIntraQP ) || m_pcEncCfg->m_RCRateControlMode != 1 )
  {
    estQP = slice->sliceQp;
    estLambda = m_pcEncCfg->m_RCRateControlMode == 3 ? m_cRdCost.getLambda() : m_pcRateCtrl->encRCPic->picEstLambda;
  }
  else
  {
    bpp = m_pcRateCtrl->encRCPic->getLCUTargetBpp( slice->isIRAP(), ctuRsAddr );
    if ( pic->slices[ 0 ]->isIRAP() )
    {
      estLambda = m_pcRateCtrl->encRCPic->getLCUEstLambdaAndQP( bpp, slice->sliceQp, &estQP, ctuRsAddr );
    }
    else
    {
      estLambda = m_pcRateCtrl->encRCPic->getLCUEstLambda( bpp, ctuRsAddr );
      estQP = m_pcRateCtrl->encRCPic->getLCUEstQP( estLambda, slice->sliceQp, ctuRsAddr );
    }

    estQP = Clip3( -slice->sps->qpBDOffset[ CH_L ], MAX_QP, estQP );
  }
  m_cRdCost.saveUnadjustedLambda();
  m_cRdCost.setLambda( estLambda, slice->sps->bitDepths );

  for ( uint32_t compIdx = 1; compIdx < MAX_NUM_COMP; compIdx++ )
  {
    const ComponentID compID = ComponentID( compIdx );
    int chromaQPOffset = slice->pps->chromaQpOffset[ compID ] + slice->sliceChromaQpDelta[ compID ];
    int qpc = slice->sps->chromaQpMappingTable.getMappedChromaQpValue( compID, estQP ) + chromaQPOffset;
    double tmpWeight = pow( 2.0, ( estQP - qpc ) / 3.0 );  // takes into account of the chroma qp mapping and chroma qp Offset
    if ( m_pcEncCfg->m_DepQuantEnabled )
    {
      tmpWeight *= ( m_pcEncCfg->m_GOPSize >= 8 ? pow( 2.0, 0.1 / 3.0 ) : pow( 2.0, 0.2 / 3.0 ) );  // increase chroma weight for dependent quantization (in order to reduce bit rate shift from chroma to luma)
    }
    m_cRdCost.setDistortionWeight( compID, tmpWeight );
  }

  const double lambdaArray[ MAX_NUM_COMP ] =  { estLambda / m_cRdCost.getDistortionWeight( COMP_Y ),
                                                estLambda / m_cRdCost.getDistortionWeight( COMP_Cb ),
                                                estLambda / m_cRdCost.getDistortionWeight( COMP_Cr ) };
  m_cTrQuant.setLambdas( lambdaArray );

  m_pcRateCtrl->rcQP = estQP;

  return;
}

void EncCu::xUpdateAfterCtuRC( CodingStructure& cs, const Slice* slice, const UnitArea& ctuArea, const double oldLambda, const int numberOfWrittenBits, const int ctuRsAddr )
{
  if ( m_pcEncCfg->m_RCRateControlMode < 1 )
  {
    return;
  }

  int actualQP = RC_INVALID_QP_VALUE;
  double actualLambda = m_cRdCost.getLambda();

  bool anyCoded = false;
  int numberOfSkipPixel = 0;
  cCUSecureTraverser trv = cs.secureTraverseCUs( ctuArea, CH_L );
  {
    const auto *cu = trv.begin;
    do
    {
      numberOfSkipPixel += cu->skip * cu->lumaSize().area();
      anyCoded          |= !cu->skip || cu->rootCbf;
    }
    while( cu != trv.last && (0!=(cu = cu->next)) );
  }
  double skipRatio = (double)numberOfSkipPixel / ctuArea.lumaSize().area();
  CodingUnit* cu = cs.getCU( ctuArea.lumaPos(), CH_L, TREE_D );

  actualQP = ( m_pcEncCfg->m_RCRateControlMode < 3 || anyCoded ) ? cu->qp : RC_INVALID_QP_VALUE;

  m_cRdCost.setLambda( oldLambda, slice->sps->bitDepths );

  int estQP = slice->sliceQp;
  for ( uint32_t compIdx = 1; compIdx < MAX_NUM_COMP; compIdx++ )
  {
    const ComponentID compID = ComponentID( compIdx );
    int chromaQPOffset = slice->pps->chromaQpOffset[ compID ] + slice->sliceChromaQpDelta[ compID ];
    int qpc = slice->sps->chromaQpMappingTable.getMappedChromaQpValue( compID, estQP ) + chromaQPOffset;
    double tmpWeight = pow( 2.0, ( estQP - qpc ) / 3.0 );  // takes into account of the chroma qp mapping and chroma qp Offset
    if ( m_pcEncCfg->m_DepQuantEnabled )
    {
      tmpWeight *= ( m_pcEncCfg->m_GOPSize >= 8 ? pow( 2.0, 0.1 / 3.0 ) : pow( 2.0, 0.2 / 3.0 ) );  // increase chroma weight for dependent quantization (in order to reduce bit rate shift from chroma to luma)
    }
    m_cRdCost.setDistortionWeight( compID, tmpWeight );
  }

  if ( m_rcMutex ) m_rcMutex->lock();

  m_pcRateCtrl->encRCPic->updateAfterCTU( ctuRsAddr, numberOfWrittenBits, actualQP, actualLambda, skipRatio,
    slice->isIRAP() ? 0 : m_pcEncCfg->m_RCRateControlMode == 1 );

  if ( m_rcMutex ) m_rcMutex->unlock();

  return;
}

void EncCu::xCheckDQP( CodingStructure& cs, Partitioner& partitioner, bool bKeepCtx )
{
  if( !cs.pps->useDQP )
  {
    return;
  }

  if (partitioner.isSepTree(cs) && isChroma(partitioner.chType))
  {
    return;
  }

  if( !partitioner.currQgEnable() ) // do not consider split or leaf/not leaf QG condition (checked by caller)
  {
    return;
  }

  CodingUnit* cuFirst = cs.getCU( partitioner.chType, partitioner.treeType );

  CHECK( bKeepCtx && cs.cus.size() <= 1 && partitioner.getImplicitSplit( cs ) == CU_DONT_SPLIT, "bKeepCtx should only be set in split case" );
  CHECK( !bKeepCtx && cs.cus.size() > 1, "bKeepCtx should never be set for non-split case" );
  CHECK( !cuFirst, "No CU available" );

  bool hasResidual = false;
  for( const auto &cu : cs.cus )
  {
    //not include the chroma CU because chroma CU is decided based on corresponding luma QP and deltaQP is not signaled at chroma CU
    if( cu->rootCbf && !isChroma( cu->chType ))
    {
      hasResidual = true;
      break;
    }
  }

  int predQP = CU::predictQP( *cuFirst, cs.prevQP[partitioner.chType] );

  if( hasResidual )
  {
    TempCtx ctxTemp( m_CtxCache );
    if( !bKeepCtx ) ctxTemp = SubCtx( Ctx::DeltaQP, m_CABACEstimator->getCtx() );

    m_CABACEstimator->resetBits();
    m_CABACEstimator->cu_qp_delta( *cuFirst, predQP, cuFirst->qp );

    cs.fracBits += m_CABACEstimator->getEstFracBits(); // dQP bits
    cs.cost      = m_cRdCost.calcRdCost(cs.fracBits, cs.dist);


    if( !bKeepCtx ) m_CABACEstimator->getCtx() = SubCtx( Ctx::DeltaQP, ctxTemp );

    // NOTE: reset QPs for CUs without residuals up to first coded CU
    for( const auto &cu : cs.cus )
    {
      //not include the chroma CU because chroma CU is decided based on corresponding luma QP and deltaQP is not signaled at chroma CU
      if( cu->rootCbf && !isChroma( cu->chType ))
      {
        break;
      }
      cu->qp = predQP;
    }
  }
  else
  {
    // No residuals: reset CU QP to predicted value
    for( const auto &cu : cs.cus )
    {
      cu->qp = predQP;
    }
  }
}

void EncCu::xCheckRDCostMerge( CodingStructure *&tempCS, CodingStructure *&bestCS, Partitioner &partitioner, EncTestMode& encTestMode )
{
  PROFILER_SCOPE_AND_STAGE_EXT( 1, g_timeProfiler, P_INTER_MRG, tempCS, partitioner.chType );
  const Slice &slice = *tempCS->slice;
  const SPS& sps = *tempCS->sps;

  CHECK( slice.sliceType == I_SLICE, "Merge modes not available for I-slices" );

  tempCS->initStructData( encTestMode.qp );

  MergeCtx mergeCtx;
  if (sps.SbtMvp)
  {
    Size bufSize = g_miScaling.scale(tempCS->area.lumaSize());
    mergeCtx.subPuMvpMiBuf = MotionBuf(m_subPuMiBuf, bufSize);
  }
  Mv   refinedMvdL0[MRG_MAX_NUM_CANDS][MAX_NUM_SUBCU_DMVR];

  m_mergeBestSATDCost = MAX_DOUBLE;

  bool mrgTempBufSet = false;

  PelUnitBuf* globSortedPelBuf[MRG_MAX_NUM_CANDS];
  bool mmvdCandInserted = false;

  {
    // first get merge candidates
    CodingUnit cu( tempCS->area );
    cu.cs       = tempCS;
    cu.predMode = MODE_INTER;
    cu.slice    = tempCS->slice;
    cu.tileIdx  = 0;

    PredictionUnit pu( tempCS->area );
    pu.cu = &cu;
    pu.cs = tempCS;
    PU::getInterMergeCandidates(pu, mergeCtx, 0 );
    PU::getInterMMVDMergeCandidates(pu, mergeCtx);

    pu.regularMergeFlag = true;
  }

  bool      candHasNoResidual[MRG_MAX_NUM_CANDS + MMVD_ADD_NUM] = { false };
  bool      bestIsSkip = false;
  bool      bestIsMMVDSkip = true;
  unsigned  uiNumMrgSATDCand = mergeCtx.numValidMergeCand + MMVD_ADD_NUM;

  struct ModeInfo
  {
    uint32_t mergeCand;
    bool     isRegularMerge;
    bool     isMMVD;
    bool     isCIIP;
    bool     isBioOrDmvr;
    ModeInfo() {}
    ModeInfo(const uint32_t mergeCand, const bool isRegularMerge, const bool isMMVD, const bool isCIIP, const bool BioOrDmvr) :
      mergeCand(mergeCand), isRegularMerge(isRegularMerge), isMMVD(isMMVD), isCIIP(isCIIP), isBioOrDmvr(BioOrDmvr) {}
  };

  static_vector<ModeInfo, MRG_MAX_NUM_CANDS + MMVD_ADD_NUM>  RdModeList;
  const int candNum = mergeCtx.numValidMergeCand + (sps.MMVD ? std::min<int>(MMVD_BASE_MV_NUM, mergeCtx.numValidMergeCand) * MMVD_MAX_REFINE_NUM : 0);

  for (int i = 0; i < candNum; i++)
  {
    if (i < mergeCtx.numValidMergeCand)
    {
      RdModeList.push_back(ModeInfo(i, true, false, false, false));
    }
    else
    {
      RdModeList.push_back(ModeInfo(std::min(MMVD_ADD_NUM, i - mergeCtx.numValidMergeCand), false, true, false, false));
    }
  }

  bool fastCIIP  = m_pcEncCfg->m_CIIP>1;
  bool testCIIP  = sps.CIIP && (bestCS->area.lwidth() * bestCS->area.lheight() >= 64 && bestCS->area.lwidth() < MAX_CU_SIZE && bestCS->area.lheight() < MAX_CU_SIZE);
       testCIIP &= (!fastCIIP) || !m_modeCtrl.getBlkInfo( tempCS->area ).isSkip; //5
  int numCiipIntra = 0;
  const ReshapeData& reshapeData = tempCS->picture->reshapeData;
  PelUnitBuf ciipBuf = m_aTmpStorageLCU[1].getCompactBuf( tempCS->area );

  m_SortedPelUnitBufs.reset();

  const uint16_t merge_ctx_size = Ctx::MergeFlag.size() + Ctx::RegularMergeFlag.size() + Ctx::MergeIdx.size() + Ctx::MmvdFlag.size() + Ctx::MmvdMergeIdx.size() + Ctx::MmvdStepMvpIdx.size() + Ctx::SubblockMergeFlag.size() + Ctx::AffMergeIdx.size() + Ctx::CiipFlag.size();
  const TempCtx  ctxStartIntraCtx( m_CtxCache, SubCtx(CtxSet(Ctx::MergeFlag(), merge_ctx_size), m_CABACEstimator->getCtx()) );
  int numCiiPExtraTests = fastCIIP ? 0 : 1;

  if( m_pcEncCfg->m_useFastMrg || testCIIP )
  {
    uiNumMrgSATDCand = NUM_MRG_SATD_CAND + (testCIIP ? numCiiPExtraTests : 0);
    bestIsSkip = !testCIIP && m_modeCtrl.getBlkInfo( tempCS->area ).isSkip;
    bestIsMMVDSkip = m_modeCtrl.getBlkInfo(tempCS->area).isMMVDSkip;

    static_vector<double, MRG_MAX_NUM_CANDS + MMVD_ADD_NUM> candCostList;
    Distortion uiSadBestForQPA = MAX_DISTORTION;
    // 1. Pass: get SATD-cost for selected candidates and reduce their count
    if( !bestIsSkip )
    {
      const UnitArea localUnitArea(tempCS->area.chromaFormat, Area(0, 0, tempCS->area.Y().width, tempCS->area.Y().height));
      m_SortedPelUnitBufs.prepare(localUnitArea, uiNumMrgSATDCand+4);

      mrgTempBufSet = true;
      RdModeList.clear();
      m_CABACEstimator->getCtx() = SubCtx(CtxSet(Ctx::MergeFlag(), merge_ctx_size), ctxStartIntraCtx);

      CodingUnit &cu      = tempCS->addCU( tempCS->area, partitioner.chType );
      const double sqrtLambdaForFirstPassIntra = m_cRdCost.getMotionLambda() * FRAC_BITS_SCALE;
      partitioner.setCUData( cu );
      cu.slice        = tempCS->slice;
      cu.tileIdx      = 0;
      cu.skip         = false;
      cu.mmvdSkip     = false;
      cu.geo          = false;
      cu.predMode     = MODE_INTER;
      cu.chromaQpAdj  = m_cuChromaQpOffsetIdxPlus1;
      cu.qp           = encTestMode.qp;
    //cu.emtFlag  is set below

      PredictionUnit &pu  = tempCS->addPU( cu, partitioner.chType, &cu );

      const DFunc dfunc = encTestMode.lossless || tempCS->slice->disableSATDForRd ? DF_SAD : DF_HAD;
      DistParam distParam = m_cRdCost.setDistParam(tempCS->getOrgBuf(COMP_Y), m_SortedPelUnitBufs.getTestBuf(COMP_Y), sps.bitDepths[ CH_L ],  dfunc);

      bool sameMV[ MRG_MAX_NUM_CANDS ] = { false };
      if (m_pcEncCfg->m_useFastMrg == 2)
      {
        for (int m = 0; m < mergeCtx.numValidMergeCand - 1; m++)
        {
          if( sameMV[m] == false)
          {
            for (int n = m + 1; n < mergeCtx.numValidMergeCand; n++)
            {
              if( (mergeCtx.mvFieldNeighbours[(m << 1) + 0].mv == mergeCtx.mvFieldNeighbours[(n << 1) + 0].mv)
               && (mergeCtx.mvFieldNeighbours[(m << 1) + 1].mv == mergeCtx.mvFieldNeighbours[(n << 1) + 1].mv))
              {
                sameMV[n] = true;
              }
            }
          }
        }
      }

      for( uint32_t uiMergeCand = 0; uiMergeCand < mergeCtx.numValidMergeCand; uiMergeCand++ )
      {
        if (sameMV[uiMergeCand])
        {
          continue;
        }
        mergeCtx.setMergeInfo( pu, uiMergeCand );

        PU::spanMotionInfo( pu, mergeCtx );
        pu.mvRefine = true;
        bool BioOrDmvr = m_cInterSearch.motionCompensation(pu, m_SortedPelUnitBufs.getTestBuf(), REF_PIC_LIST_X);
        pu.mvRefine = false;

        if( mergeCtx.interDirNeighbours[uiMergeCand] == 3 && mergeCtx.mrgTypeNeighbours[uiMergeCand] == MRG_TYPE_DEFAULT_N )
        {
          mergeCtx.mvFieldNeighbours[2*uiMergeCand].mv   = pu.mv[0];
          mergeCtx.mvFieldNeighbours[2*uiMergeCand+1].mv = pu.mv[1];
          {
            if (PU::checkDMVRCondition(pu))
            {
              int num = 0;
              for (int i = 0; i < (pu.lumaSize().height); i += DMVR_SUBCU_SIZE)
              {
                for (int j = 0; j < (pu.lumaSize().width); j += DMVR_SUBCU_SIZE)
                {
                  refinedMvdL0[uiMergeCand][num] = pu.mvdL0SubPu[num];
                  num++;
                }
              }
            }
          }
        }
        distParam.cur.buf = m_SortedPelUnitBufs.getTestBuf().Y().buf;

        Distortion uiSad = distParam.distFunc(distParam);
        uint64_t fracBits = xCalcPuMeBits(pu);

        //restore ctx
        m_CABACEstimator->getCtx() = SubCtx(CtxSet(Ctx::MergeFlag(), merge_ctx_size), ctxStartIntraCtx);
        if (uiSadBestForQPA > uiSad) { uiSadBestForQPA = uiSad; }
        double cost = (double)uiSad + (double)fracBits * sqrtLambdaForFirstPassIntra;
        int insertPos = -1;
        updateCandList(ModeInfo(uiMergeCand, true, false, false, BioOrDmvr), cost, RdModeList, candCostList, uiNumMrgSATDCand, &insertPos);
        m_SortedPelUnitBufs.insert( insertPos, (int)RdModeList.size() );
        if (m_pcEncCfg->m_useFastMrg != 2)
        {
          CHECK(std::min(uiMergeCand + 1, uiNumMrgSATDCand) != RdModeList.size(), "");
        }
      }


      if (testCIIP)
      {
        unsigned numCiipInitialCand = std::min(NUM_MRG_SATD_CAND-1+numCiiPExtraTests, (const int)RdModeList.size());

        //save trhe original order
        uint32_t sortedMergeCand[4];
        bool     BioOrDmvr[4];
        int numCiipTests = 0;
        for (uint32_t mergeCounter = 0; mergeCounter < numCiipInitialCand; mergeCounter++)
        {
          if (!sameMV[mergeCounter] && ( m_pcEncCfg->m_CIIP != 3 || !RdModeList[mergeCounter].isBioOrDmvr ) )
          {
            globSortedPelBuf[RdModeList[mergeCounter].mergeCand] = m_SortedPelUnitBufs.getBufFromSortedList( mergeCounter );
            sortedMergeCand[numCiipTests] = RdModeList[mergeCounter].mergeCand;
            BioOrDmvr[numCiipTests] = RdModeList[mergeCounter].isBioOrDmvr;
            numCiipTests++;
          }
        }

        if( numCiipTests )
        {
          pu.ciip = true;
          // generate intrainter Y prediction
          pu.intraDir[0] = PLANAR_IDX;
          m_cIntraSearch.initIntraPatternChType(*pu.cu, pu.Y());
          m_cIntraSearch.predIntraAng(COMP_Y, ciipBuf.Y(), pu);
          numCiipIntra = m_cIntraSearch.getNumIntraCiip( pu );

          // save the to-be-tested merge candidates
          for (uint32_t mergeCounter = 0; mergeCounter < numCiipTests; mergeCounter++)
          {
            uint32_t mergeCand = sortedMergeCand[mergeCounter];

            // estimate merge bits
            mergeCtx.setMergeInfo(pu, mergeCand);

            PelUnitBuf testBuf = m_SortedPelUnitBufs.getTestBuf();

            if( BioOrDmvr[mergeCounter] ) // recalc
            {
              pu.mvRefine = false;
              pu.mcControl = 0;
              m_cInterSearch.motionCompensation(pu, testBuf);
            }
            else
            {
              testBuf.copyFrom( *globSortedPelBuf[mergeCand] );
            }

            if( slice.lmcsEnabled && reshapeData.getCTUFlag() )
            {
              testBuf.Y().rspSignal( reshapeData.getFwdLUT());
            }
            testBuf.Y().weightCiip( ciipBuf.Y(), numCiipIntra );

            // calculate cost
            if( slice.lmcsEnabled && reshapeData.getCTUFlag() )
            {
              PelBuf tmpLmcs = m_aTmpStorageLCU[0].getCompactBuf( pu.Y() );
              tmpLmcs.rspSignal( testBuf.Y(), reshapeData.getInvLUT());
              distParam.cur = tmpLmcs;
            }
            else
            {
              distParam.cur = testBuf.Y();
            }

            Distortion sadValue = distParam.distFunc(distParam);

            m_CABACEstimator->getCtx() = SubCtx(CtxSet(Ctx::MergeFlag(), merge_ctx_size), ctxStartIntraCtx);
            pu.regularMergeFlag = false;
            uint64_t fracBits = xCalcPuMeBits(pu);
            if (uiSadBestForQPA > sadValue) { uiSadBestForQPA = sadValue; }
            double cost = (double)sadValue + (double)fracBits * sqrtLambdaForFirstPassIntra;
            int insertPos = -1;

            updateCandList(ModeInfo(mergeCand, false, false, true, false), cost, RdModeList, candCostList, uiNumMrgSATDCand, &insertPos);
            if( insertPos > -1 )
            {
              m_SortedPelUnitBufs.getTestBuf().copyFrom( testBuf );
              m_SortedPelUnitBufs.insert( insertPos, uiNumMrgSATDCand+4 ); // add 4 to prevent best RdCandidates being reused as testbuf
            }
            else if (fastCIIP) //3
            {
              break;
            }
          }
          pu.ciip = false;
        }
      }

      bool testMMVD = true;
      if (m_pcEncCfg->m_useFastMrg == 2)
      {
        uiNumMrgSATDCand = (unsigned)RdModeList.size();
        testMMVD = (RdModeList.size() > 1);
      }
      if (pu.cs->sps->MMVD && testMMVD)
      {
        cu.mmvdSkip = true;
        pu.regularMergeFlag = true;
        int tempNum = (mergeCtx.numValidMergeCand > 1) ? MMVD_ADD_NUM : MMVD_ADD_NUM >> 1;

        int bestDir = 0;
        double bestCostMerge = candCostList[uiNumMrgSATDCand - 1];
        double bestCostOffset = MAX_DOUBLE;
        bool doMMVD = true;
        int shiftCandStart = 0;

        if (m_pcEncCfg->m_MMVD == 4)
        {
          if (RdModeList[0].mergeCand > 1 && RdModeList[1].mergeCand > 1)
          {
            doMMVD = false;
          }
          else if (!(RdModeList[0].mergeCand < 2 && RdModeList[1].mergeCand < 2))
          {
            int shiftCand = RdModeList[0].mergeCand < 2 ? RdModeList[0].mergeCand : RdModeList[1].mergeCand;
            if (shiftCand)
            {
              shiftCandStart = MMVD_MAX_REFINE_NUM;
            }
            else
            {
              tempNum = MMVD_MAX_REFINE_NUM;
            }
          }
        }
        for (uint32_t mmvdMergeCand = shiftCandStart; (mmvdMergeCand < tempNum) && doMMVD; mmvdMergeCand++)
        {
          if (m_pcEncCfg->m_MMVD > 1)
          {
            int checkMMVD = xCheckMMVDCand(mmvdMergeCand, bestDir, tempNum, bestCostOffset, bestCostMerge, candCostList[uiNumMrgSATDCand - 1]);
            if (checkMMVD)
            {
              if (checkMMVD == 2)
              {
                break;
              }
              continue;
            }
          }
          int baseIdx = mmvdMergeCand / MMVD_MAX_REFINE_NUM;

          int refineStep = (mmvdMergeCand - (baseIdx * MMVD_MAX_REFINE_NUM)) / 4;
          if (refineStep >= m_pcEncCfg->m_MmvdDisNum )
          {
            continue;
          }
          mergeCtx.setMmvdMergeCandiInfo(pu, mmvdMergeCand);

          PU::spanMotionInfo(pu, mergeCtx);
          pu.mvRefine = true;
          pu.mcControl = (refineStep > 2) || (m_pcEncCfg->m_MMVD > 1) ? 3 : 0;
          CHECK(!pu.mmvdMergeFlag, "MMVD merge should be set");
          // Don't do chroma MC here
          m_cInterSearch.motionCompensation(pu, m_SortedPelUnitBufs.getTestBuf(), REF_PIC_LIST_X);
          pu.mcControl = 0;
          pu.mvRefine = false;
          distParam.cur.buf = m_SortedPelUnitBufs.getTestBuf().Y().buf;
          Distortion uiSad = distParam.distFunc(distParam);

          m_CABACEstimator->getCtx() = SubCtx(CtxSet(Ctx::MergeFlag(), merge_ctx_size), ctxStartIntraCtx);
          uint64_t fracBits = xCalcPuMeBits(pu);
          if (uiSadBestForQPA > uiSad) { uiSadBestForQPA = uiSad; }
          double cost = (double)uiSad + (double)fracBits * sqrtLambdaForFirstPassIntra;
          if (m_pcEncCfg->m_MMVD > 1 && bestCostOffset > cost)
          {
            bestCostOffset = cost;
            int CandCur = mmvdMergeCand - MMVD_MAX_REFINE_NUM*baseIdx;
            if (CandCur < 4)
            {
              bestDir = CandCur;
            }
          }
          int insertPos = -1;
          updateCandList(ModeInfo(mmvdMergeCand, false, true, false, false), cost, RdModeList, candCostList, uiNumMrgSATDCand, &insertPos);
          mmvdCandInserted |= insertPos>-1;
          m_SortedPelUnitBufs.insert(insertPos, (int)RdModeList.size());
        }
      }

      // Try to limit number of candidates using SATD-costs
      uiNumMrgSATDCand = (m_pcEncCfg->m_useFastMrg == 2) ? (unsigned)candCostList.size() : uiNumMrgSATDCand;
      for( uint32_t i = 1; i < uiNumMrgSATDCand; i++ )
      {
        if( candCostList[i] > MRG_FAST_RATIO * candCostList[0] )
        {
          uiNumMrgSATDCand = i;
          break;
        }
      }
      m_mergeBestSATDCost = candCostList[0];
      if (testCIIP && isChromaEnabled(pu.cs->pcv->chrFormat) && pu.chromaSize().width != 2 )
      {
        for (uint32_t mergeCnt = 0; mergeCnt < uiNumMrgSATDCand; mergeCnt++)
        {
          if (RdModeList[mergeCnt].isCIIP)
          {
            pu.intraDir[0] = PLANAR_IDX;
            pu.intraDir[1] = DM_CHROMA_IDX;
            pu.ciip = true;

            m_cIntraSearch.initIntraPatternChType(*pu.cu, pu.Cb());
            m_cIntraSearch.predIntraAng(COMP_Cb, ciipBuf.Cb(), pu);

            m_cIntraSearch.initIntraPatternChType(*pu.cu, pu.Cr());
            m_cIntraSearch.predIntraAng(COMP_Cr, ciipBuf.Cr(), pu);

            pu.ciip = false;
            break;
          }
        }
      }

      tempCS->initStructData( encTestMode.qp );
      m_CABACEstimator->getCtx() = SubCtx(CtxSet(Ctx::MergeFlag(), merge_ctx_size), ctxStartIntraCtx);
    }
    else
    {
      if (m_pcEncCfg->m_useFastMrg != 2)
      {
        if (bestIsMMVDSkip)
        {
          uiNumMrgSATDCand = mergeCtx.numValidMergeCand + ((mergeCtx.numValidMergeCand > 1) ? MMVD_ADD_NUM : MMVD_ADD_NUM >> 1);
        }
        else
        {
          uiNumMrgSATDCand = mergeCtx.numValidMergeCand;
        }
      }
    }

    if (m_pcEncCfg->m_usePerceptQPATempFiltISlice && (uiSadBestForQPA < MAX_DISTORTION) && (slice.TLayer == 0) // non-Intra key-frame
        && partitioner.currQgEnable() && (partitioner.currSubdiv == 0)) // CTU-level luma quantization group
    {
      const Picture*    pic = slice.pic;
      const uint32_t rsAddr = getCtuAddr (partitioner.currQgPos, *pic->cs->pcv);
      const int pumpReducQP = BitAllocation::getCtuPumpingReducingQP (&slice, tempCS->getOrgBuf (COMP_Y), uiSadBestForQPA, *m_globalCtuQpVector, rsAddr, m_pcEncCfg->m_QP);

      if (pumpReducQP != 0) // subtract QP offset, reduces Intra-period pumping or overcoding
      {
        encTestMode.qp = Clip3 (0, MAX_QP, encTestMode.qp - pumpReducQP);
        tempCS->currQP[partitioner.chType] = tempCS->baseQP =
        bestCS->currQP[partitioner.chType] = bestCS->baseQP = Clip3 (0, MAX_QP, tempCS->baseQP - pumpReducQP);

        updateLambda (slice, pic->ctuQpaLambda[rsAddr], pic->ctuAdaptedQP[rsAddr], tempCS->baseQP, true);
      }
    }
  }

  uint32_t iteration = (encTestMode.lossless) ? 1 : 2;

  double bestEndCost = MAX_DOUBLE;
  bool isTestSkipMerge[MRG_MAX_NUM_CANDS] = {false}; // record if the merge candidate has tried skip mode

  for (uint32_t uiNoResidualPass = 0; uiNoResidualPass < iteration; ++uiNoResidualPass)
  {
    for( uint32_t uiMrgHADIdx = 0; uiMrgHADIdx < uiNumMrgSATDCand; uiMrgHADIdx++ )
    {
      uint32_t uiMergeCand = RdModeList[uiMrgHADIdx].mergeCand;

      if (uiNoResidualPass != 0 && RdModeList[uiMrgHADIdx].isCIIP) // intrainter does not support skip mode
      {
        if (isTestSkipMerge[uiMergeCand])
        {
          continue;
        }
      }

      if (((uiNoResidualPass != 0) && candHasNoResidual[uiMrgHADIdx])
       || ( (uiNoResidualPass == 0) && bestIsSkip ) )
      {
        continue;
      }

      // first get merge candidates
      CodingUnit &cu      = tempCS->addCU( tempCS->area, partitioner.chType );

      partitioner.setCUData( cu );
      cu.slice        = tempCS->slice;
      cu.tileIdx      = 0;
      cu.skip         = false;
      cu.mmvdSkip     = false;
      cu.geo          = false;
      cu.predMode     = MODE_INTER;
      cu.chromaQpAdj  = m_cuChromaQpOffsetIdxPlus1;
      cu.qp           = encTestMode.qp;
      PredictionUnit &pu  = tempCS->addPU( cu, partitioner.chType, &cu );

      if (uiNoResidualPass == 0 && RdModeList[uiMrgHADIdx].isCIIP)
      {
        cu.mmvdSkip = false;
        mergeCtx.setMergeInfo(pu, uiMergeCand);
        pu.ciip             = true;
        pu.regularMergeFlag = false;
        pu.intraDir[0]      = PLANAR_IDX;
        pu.intraDir[1]      = DM_CHROMA_IDX;
      }
      else if (RdModeList[uiMrgHADIdx].isMMVD)
      {
        cu.mmvdSkip         = true;
        pu.regularMergeFlag = true;
        pu.ciip             = false;
        mergeCtx.setMmvdMergeCandiInfo(pu, uiMergeCand);
      }
      else
      {
        cu.mmvdSkip         = false;
        pu.regularMergeFlag = true;
        pu.ciip             = false;
        mergeCtx.setMergeInfo(pu, uiMergeCand);
      }

      PU::spanMotionInfo( pu, mergeCtx );
      {
        if (!pu.cu->affine && pu.refIdx[0] >= 0 && pu.refIdx[1] >= 0 && (pu.lwidth() + pu.lheight() == 12)) 
        { 
          tempCS->initStructData(encTestMode.qp); 
          continue; 
        } 

        if( mrgTempBufSet )
        {
          if( PU::checkDMVRCondition( pu ) )
          {
            int num = 0;
            for( int i = 0; i < ( pu.lumaSize().height ); i += DMVR_SUBCU_SIZE )
            {
              for( int j = 0; j < ( pu.lumaSize().width ); j += DMVR_SUBCU_SIZE )
              {
                pu.mvdL0SubPu[num] = refinedMvdL0[uiMergeCand][num];
                num++;
              }
            }
          }
          if (pu.ciip)
          {
            PelUnitBuf predBuf = tempCS->getPredBuf();
            predBuf.copyFrom( *m_SortedPelUnitBufs.getBufFromSortedList( uiMrgHADIdx ));

            if (isChromaEnabled(pu.chromaFormat) && pu.chromaSize().width > 2 )
            {
              predBuf.Cb().weightCiip( ciipBuf.Cb(), numCiipIntra);
              predBuf.Cr().weightCiip( ciipBuf.Cr(), numCiipIntra);
            }
          }
          else
          {
            if (RdModeList[uiMrgHADIdx].isMMVD)
            {
              pu.mcControl = 0;
              m_cInterSearch.motionCompensation(pu, tempCS->getPredBuf() );
            }
            else if( RdModeList[uiMrgHADIdx].isCIIP )
            {
              if( mmvdCandInserted )
              {
                pu.mcControl = 0;
                pu.mvRefine = true;
                m_cInterSearch.motionCompensation(pu, tempCS->getPredBuf() );
                pu.mvRefine = false;
              }
              else
              {
                tempCS->getPredBuf().copyFrom( *globSortedPelBuf[uiMergeCand]);
              }
            }
            else
            {
              PelUnitBuf* sortedListBuf = m_SortedPelUnitBufs.getBufFromSortedList(uiMrgHADIdx);
              CHECK(!sortedListBuf, "Buffer failed");
              tempCS->getPredBuf().copyFrom(*sortedListBuf);
            }
          }
        }
        else
        {
          pu.mvRefine = true;
          m_cInterSearch.motionCompensation( pu, tempCS->getPredBuf() );
          pu.mvRefine = false;
        }
      }
      if (!cu.mmvdSkip && !pu.ciip && uiNoResidualPass != 0)
      {
        CHECK(uiMergeCand >= mergeCtx.numValidMergeCand, "out of normal merge");
        isTestSkipMerge[uiMergeCand] = true;
      }

      xEncodeInterResidual( tempCS, bestCS, partitioner, encTestMode, uiNoResidualPass, uiNoResidualPass == 0 ? &candHasNoResidual[uiMrgHADIdx] : NULL );

      if (m_pcEncCfg->m_useFastMrg == 2)
      {
        if( pu.ciip && bestCS->cost == MAX_DOUBLE && uiMrgHADIdx+1 == uiNumMrgSATDCand )
        {
          uiNumMrgSATDCand = (unsigned)RdModeList.size();
        }

        if (uiMrgHADIdx > 0 && tempCS->cost >= bestEndCost && !pu.ciip)
        {
          uiMrgHADIdx = uiNumMrgSATDCand;
          tempCS->initStructData(encTestMode.qp);
          continue;
        }
        if (uiNoResidualPass == 0 && tempCS->cost < bestEndCost)
        {
          bestEndCost = tempCS->cost;
        }
      }

      if( m_pcEncCfg->m_useFastDecisionForMerge && !bestIsSkip && !pu.ciip)
      {
        bestIsSkip = !bestCS->cus.empty() && bestCS->getCU( partitioner.chType, partitioner.treeType )->rootCbf == 0;
      }
      tempCS->initStructData( encTestMode.qp );
    }// end loop uiMrgHADIdx

    if( uiNoResidualPass == 0 && m_pcEncCfg->m_useEarlySkipDetection )
    {
      const CodingUnit     &bestCU = *bestCS->getCU( partitioner.chType, partitioner.treeType );
      const PredictionUnit &bestPU = *bestCS->getPU( partitioner.chType );

      if( bestCU.rootCbf == 0 )
      {
        if( bestPU.mergeFlag )
        {
          m_modeCtrl.comprCUCtx->earlySkip = true;
        }
        else if( m_pcEncCfg->m_motionEstimationSearchMethod != MESEARCH_SELECTIVE )
        {
          int absolute_MV = 0;

          for( uint32_t uiRefListIdx = 0; uiRefListIdx < 2; uiRefListIdx++ )
          {
            if( slice.numRefIdx[ uiRefListIdx ] > 0 )
            {
              absolute_MV += bestPU.mvd[uiRefListIdx].getAbsHor() + bestPU.mvd[uiRefListIdx].getAbsVer();
            }
          }

          if( absolute_MV == 0 )
          {
            m_modeCtrl.comprCUCtx->earlySkip = true;
          }
        }
      }
    }
  }
  STAT_COUNT_CU_MODES( partitioner.chType == CH_L, g_cuCounters1D[CU_MODES_TESTED][0][!tempCS->slice->isIntra() + tempCS->slice->depth] );
  STAT_COUNT_CU_MODES( partitioner.chType == CH_L && !tempCS->slice->isIntra(), g_cuCounters2D[CU_MODES_TESTED][Log2( tempCS->area.lheight() )][Log2( tempCS->area.lwidth() )] );
}


void EncCu::xCheckRDCostMergeGeo(CodingStructure *&tempCS, CodingStructure *&bestCS, Partitioner &pm, const EncTestMode &encTestMode)
{
  PROFILER_SCOPE_AND_STAGE_EXT( 1, g_timeProfiler, P_INTER_MRG, tempCS, partitioner.chType );

  const Slice &slice = *tempCS->slice;
  if ((m_pcEncCfg->m_Geo > 1) && (slice.TLayer <= 1))
  {
    return;
  }

  tempCS->initStructData(encTestMode.qp);

  MergeCtx   mergeCtx;
  const SPS &sps = *tempCS->sps;

  if (sps.SbtMvp)
  {
    Size bufSize           = g_miScaling.scale(tempCS->area.lumaSize());
    mergeCtx.subPuMvpMiBuf = MotionBuf(m_subPuMiBuf, bufSize);
  }
  CodingUnit &cu = tempCS->addCU(tempCS->area, pm.chType);
  pm.setCUData(cu);
  cu.predMode  = MODE_INTER;
  cu.slice     = tempCS->slice;
  cu.tileIdx   = 0;
  cu.qp        = encTestMode.qp;
  cu.affine    = false;
  cu.mtsFlag   = false;
  cu.BcwIdx    = BCW_DEFAULT;
  cu.geo       = true;
  cu.imv       = 0;
  cu.mmvdSkip  = false;
  cu.skip      = false;
  cu.mipFlag   = false;
  cu.bdpcmMode = 0;

  PredictionUnit &pu  = tempCS->addPU(cu, pm.chType, &cu);
  pu.mergeFlag        = true;
  pu.regularMergeFlag = false;
  PU::getGeoMergeCandidates(pu, mergeCtx);

  GeoComboCostList comboList;
  int              bitsCandTB = floorLog2(GEO_NUM_PARTITION_MODE);
  PelUnitBuf       geoCombinations[GEO_MAX_TRY_WEIGHTED_SAD];
  DistParam        distParam;

  const UnitArea   localUnitArea(tempCS->area.chromaFormat, Area(0, 0, tempCS->area.Y().width, tempCS->area.Y().height));
  const double     sqrtLambdaForFirstPass = m_cRdCost.getMotionLambda();

  uint8_t   maxNumMergeCandidates = cu.cs->sps->maxNumGeoCand;
  m_SortedPelUnitBufs.prepare(localUnitArea, GEO_MAX_TRY_WEIGHTED_SATD );
  DistParam distParamWholeBlk;

  m_cRdCost.setDistParam(distParamWholeBlk, tempCS->getOrgBuf().Y(), m_SortedPelUnitBufs.getTestBuf().Y().buf, m_SortedPelUnitBufs.getTestBuf().Y().stride, sps.bitDepths[CH_L], COMP_Y);
  Distortion bestWholeBlkSad  = MAX_UINT64;
  double     bestWholeBlkCost = MAX_DOUBLE;
  Distortion sadWholeBlk[ GEO_MAX_NUM_UNI_CANDS];

  if (m_pcEncCfg->m_Geo == 3)
  {
    maxNumMergeCandidates = maxNumMergeCandidates > 1 ? ((maxNumMergeCandidates >> 1) + 1) : maxNumMergeCandidates;
  }

  int PermitCandidates = maxNumMergeCandidates-1;
  {
    // NOTE: Diagnostic is disabled due to a GCC bug (7.4.0).
    //       GCC is trying to optimize the loop and complains about the possible exceeding of array bounds
#if FIX_FOR_TEMPORARY_COMPILER_ISSUES_ENABLED && GCC_VERSION_AT_LEAST(7,3)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds"
#endif
    int        pocMrg[ GEO_MAX_NUM_UNI_CANDS];
    Mv         MrgMv [ GEO_MAX_NUM_UNI_CANDS];
    for (uint8_t mergeCand = 0; mergeCand < maxNumMergeCandidates; mergeCand++)
    {
      int        MrgList   = mergeCtx.mvFieldNeighbours[(mergeCand << 1) + 0].refIdx == -1 ? 1 : 0;
      RefPicList eList     = (MrgList ? REF_PIC_LIST_1 : REF_PIC_LIST_0);
      int        MrgrefIdx = mergeCtx.mvFieldNeighbours[(mergeCand << 1) + MrgList].refIdx;
      pocMrg[mergeCand]    = tempCS->slice->getRefPic(eList, MrgrefIdx)->getPOC();
      MrgMv[mergeCand]     = mergeCtx.mvFieldNeighbours[(mergeCand << 1) + MrgList].mv;
      if (mergeCand)
      {
        for (int i = 0; i < mergeCand; i++)
        {
          if (pocMrg[mergeCand] == pocMrg[i] && MrgMv[mergeCand] == MrgMv[i])
          {
            PermitCandidates--;
            break;
          }
        }
      }
    }
#if FIX_FOR_TEMPORARY_COMPILER_ISSUES_ENABLED && GCC_VERSION_AT_LEAST(7,3)
#pragma GCC diagnostic pop
#endif
  }

  if (PermitCandidates<=0)
  {
    return;
  }

  bool sameMV[MRG_MAX_NUM_CANDS] = { false };
  if (m_pcEncCfg->m_Geo > 1)
  {
    for (int m = 0; m < maxNumMergeCandidates; m++)
    {
      if (sameMV[m] == false)
      {
        for (int n = m + 1; n < maxNumMergeCandidates; n++)
        {
          if( (mergeCtx.mvFieldNeighbours[(m << 1) + 0].mv == mergeCtx.mvFieldNeighbours[(n << 1) + 0].mv)
           && (mergeCtx.mvFieldNeighbours[(m << 1) + 1].mv == mergeCtx.mvFieldNeighbours[(n << 1) + 1].mv))
          {
            sameMV[n] = true;
          }
        }
      }
    }
  }

  PelUnitBuf mcBuf[MAX_TMP_BUFS];
  PelBuf    sadBuf[MAX_TMP_BUFS];
  for( int i = 0; i < maxNumMergeCandidates; i++)
  {
    mcBuf[i]  = m_aTmpStorageLCU[i].getCompactBuf( pu );
    sadBuf[i] = m_SortedPelUnitBufs.getBufFromSortedList(i)->Y();
  }

  {
    const ClpRng& lclpRng = cu.slice->clpRngs[COMP_Y];
    const unsigned rshift  = std::max<int>(2, (IF_INTERNAL_PREC - lclpRng.bd));
    const int offset = (1 << (rshift - 1)) + IF_INTERNAL_OFFS;
    const int numSamples = cu.lwidth() * cu.lheight();
    for (uint8_t mergeCand = 0; mergeCand < maxNumMergeCandidates; mergeCand++)
    {
      if (sameMV[mergeCand] )
      {
        continue;
      }

      mergeCtx.setMergeInfo(pu, mergeCand);
      PU::spanMotionInfo(pu, mergeCtx);
      m_cInterSearch.motionCompensation(pu, mcBuf[mergeCand], REF_PIC_LIST_X); //new

      g_pelBufOP.roundGeo( mcBuf[mergeCand].Y().buf, sadBuf[mergeCand].buf, numSamples, rshift, offset, lclpRng);

      distParamWholeBlk.cur.buf = sadBuf[mergeCand].buf;
      sadWholeBlk[mergeCand]    = distParamWholeBlk.distFunc(distParamWholeBlk);
      if (sadWholeBlk[mergeCand] < bestWholeBlkSad)
      {
        bestWholeBlkSad  = sadWholeBlk[mergeCand];
        int bitsCand     = mergeCand + 1;
        bestWholeBlkCost = (double) bestWholeBlkSad + (double) bitsCand * sqrtLambdaForFirstPass;
      }
    }
  }
  int wIdx = floorLog2(cu.lwidth()) - GEO_MIN_CU_LOG2;
  int hIdx = floorLog2(cu.lheight()) - GEO_MIN_CU_LOG2;

  for (int splitDir = 0; splitDir < GEO_NUM_PARTITION_MODE;)
  {
    int maskStride = 0, maskStride2 = 0;
    int stepX = 1;
    Pel *SADmask;
    int16_t angle = g_GeoParams[splitDir][0];
    if (g_angle2mirror[angle] == 2)
    {
      maskStride = -GEO_WEIGHT_MASK_SIZE;
      maskStride2 = -(int) cu.lwidth();
      SADmask = &g_globalGeoEncSADmask[g_angle2mask[g_GeoParams[splitDir][0]]]
                                      [(GEO_WEIGHT_MASK_SIZE - 1 - g_weightOffset[hIdx][wIdx][splitDir][1])
                                       * GEO_WEIGHT_MASK_SIZE + g_weightOffset[hIdx][wIdx][splitDir][0]];
    }
    else if (g_angle2mirror[angle] == 1)
    {
      stepX = -1;
      maskStride2 = cu.lwidth();
      maskStride = GEO_WEIGHT_MASK_SIZE;
      SADmask = &g_globalGeoEncSADmask[g_angle2mask[g_GeoParams[splitDir][0]]]
                                      [g_weightOffset[hIdx][wIdx][splitDir][1] * GEO_WEIGHT_MASK_SIZE
                                       + (GEO_WEIGHT_MASK_SIZE - 1 - g_weightOffset[hIdx][wIdx][splitDir][0])];
    }
    else
    {
      maskStride = GEO_WEIGHT_MASK_SIZE;
      maskStride2 = -(int) cu.lwidth();
      SADmask = &g_globalGeoEncSADmask[g_angle2mask[g_GeoParams[splitDir][0]]]
                                      [g_weightOffset[hIdx][wIdx][splitDir][1] * GEO_WEIGHT_MASK_SIZE
                                       + g_weightOffset[hIdx][wIdx][splitDir][0]];
    }
    Distortion sadSmall = 0, sadLarge = 0;
    m_cRdCost.setDistParamGeo(distParam, tempCS->getOrgBuf().Y(), sadBuf[0].buf, sadBuf[0].stride, SADmask, maskStride, stepX, maskStride2, sps.bitDepths[CH_L], COMP_Y);
    for (uint8_t mergeCand = 0; mergeCand < maxNumMergeCandidates; mergeCand++)
    {
      if( sameMV[mergeCand] )
      {
        continue;
      }
      int bitsCand = mergeCand + 1;

      distParam.cur.buf = sadBuf[mergeCand].buf;

      sadLarge = distParam.distFunc(distParam);
      m_GeoCostList.insert(splitDir, 0, mergeCand, (double) sadLarge + (double) bitsCand * sqrtLambdaForFirstPass);
      sadSmall = sadWholeBlk[mergeCand] - sadLarge;
      m_GeoCostList.insert(splitDir, 1, mergeCand, (double) sadSmall + (double) bitsCand * sqrtLambdaForFirstPass);
    }
    if (m_pcEncCfg->m_Geo == 3)
    {
      if (splitDir == 1)
      {
        splitDir += 7;
      }
      else if( (splitDir == 35)||((splitDir + 1) % 4))
      {
        splitDir++;
      }
      else
      {
        splitDir += 5;
      }
    }
    else
    {
      splitDir++;
    }
  }

  for (int splitDir = 0; splitDir < GEO_NUM_PARTITION_MODE; )
  {
    for (int GeoMotionIdx = 0; GeoMotionIdx < maxNumMergeCandidates * (maxNumMergeCandidates - 1); GeoMotionIdx++)
    {
      unsigned int mergeCand0 = m_GeoModeTest[GeoMotionIdx][0];
      unsigned int mergeCand1 = m_GeoModeTest[GeoMotionIdx][1];
      if( sameMV[mergeCand0] || sameMV[mergeCand1] )
      {
        continue;
      }
      double tempCost = m_GeoCostList.singleDistList[0][splitDir][mergeCand0].cost
                      + m_GeoCostList.singleDistList[1][splitDir][mergeCand1].cost;
      if (tempCost > bestWholeBlkCost)
      {
        continue;
      }
      tempCost = tempCost + (double) bitsCandTB * sqrtLambdaForFirstPass;
      comboList.list.push_back(GeoMergeCombo(splitDir, mergeCand0, mergeCand1, tempCost));
    }
    if (m_pcEncCfg->m_Geo == 3)
    {
      if (splitDir == 1)
      {
        splitDir += 7;
      }
      else if ((splitDir == 35) || ((splitDir + 1) % 4))
      {
        splitDir++;
      }
      else
      {
        splitDir += 5;
      }
    }
    else
    {
      splitDir++;
    }
  }

  if (comboList.list.empty())
  {
    return;
  }
  comboList.sortByCost();
  bool geocandHasNoResidual[GEO_MAX_TRY_WEIGHTED_SAD] = { false };
  bool                                             bestIsSkip = false;
  int                                              geoNumCobo = (int) comboList.list.size();
  static_vector<uint8_t, GEO_MAX_TRY_WEIGHTED_SAD> geoRdModeList;
  static_vector<double, GEO_MAX_TRY_WEIGHTED_SAD>  geocandCostList;

  const DFunc dfunc         = encTestMode.lossless || tempCS->slice->disableSATDForRd ? DF_SAD : DF_HAD;//new
  DistParam  distParamSAD2 = m_cRdCost.setDistParam( tempCS->getOrgBuf(COMP_Y), m_SortedPelUnitBufs.getTestBuf(COMP_Y), sps.bitDepths[CH_L], dfunc);

  int geoNumMrgSATDCand = std::min(GEO_MAX_TRY_WEIGHTED_SATD, geoNumCobo);
  const ClpRngs &clpRngs = pu.cu->slice->clpRngs;

  const int EndGeo = std::min(geoNumCobo, ((m_pcEncCfg->m_Geo > 1) ? 10 : GEO_MAX_TRY_WEIGHTED_SAD));
  for (uint8_t candidateIdx = 0; candidateIdx < EndGeo; candidateIdx++)
  {
    int splitDir   = comboList.list[candidateIdx].splitDir;
    int mergeCand0 = comboList.list[candidateIdx].mergeIdx0;
    int mergeCand1 = comboList.list[candidateIdx].mergeIdx1;

    geoCombinations[candidateIdx] = m_SortedPelUnitBufs.getTestBuf();
    m_cInterSearch.weightedGeoBlk(clpRngs, pu, splitDir, CH_L, geoCombinations[candidateIdx], mcBuf[mergeCand0], mcBuf[mergeCand1]);
    distParamSAD2.cur = geoCombinations[candidateIdx].Y();
    Distortion sad    = distParamSAD2.distFunc(distParamSAD2);
    int        mvBits = 2;
    mergeCand1 -= mergeCand1 < mergeCand0 ? 0 : 1;
    mvBits += mergeCand0;
    mvBits += mergeCand1;
    double updateCost                 = (double) sad + (double) (bitsCandTB + mvBits) * sqrtLambdaForFirstPass;
    comboList.list[candidateIdx].cost = updateCost;
    if ((m_pcEncCfg->m_Geo > 1) && candidateIdx)
    {
      if (updateCost > MRG_FAST_RATIO * geocandCostList[0] || updateCost > m_mergeBestSATDCost || updateCost > m_AFFBestSATDCost)
      {
        geoNumMrgSATDCand = (int)geoRdModeList.size();
        break;
      }
    }
    int insertPos = -1;
    updateCandList(candidateIdx, updateCost, geoRdModeList, geocandCostList, geoNumMrgSATDCand, &insertPos);
    m_SortedPelUnitBufs.insert( insertPos, geoNumMrgSATDCand );
  }
  for (uint8_t i = 0; i < geoNumMrgSATDCand; i++)
  {
    if (geocandCostList[i] > MRG_FAST_RATIO * geocandCostList[0] || geocandCostList[i] > m_mergeBestSATDCost
        || geocandCostList[i] > m_AFFBestSATDCost)
    {
      geoNumMrgSATDCand = i;
      break;
    }
  }

  if (m_pcEncCfg->m_Geo > 1)
  {
    geoNumMrgSATDCand = geoNumMrgSATDCand > 2 ? 2 : geoNumMrgSATDCand;
  }

  for (uint8_t i = 0; i < geoNumMrgSATDCand && isChromaEnabled(pu.chromaFormat); i++)
  {
    const uint8_t candidateIdx = geoRdModeList[i];
    const GeoMergeCombo& ge    = comboList.list[candidateIdx];
    m_cInterSearch.weightedGeoBlk(clpRngs, pu, ge.splitDir, CH_C, geoCombinations[candidateIdx], mcBuf[ge.mergeIdx0], mcBuf[ge.mergeIdx1]);
  }
  tempCS->initStructData(encTestMode.qp);
  uint8_t iteration;
  uint8_t iterationBegin = 0;
  iteration              = 2;

  for (uint8_t noResidualPass = iterationBegin; noResidualPass < iteration; ++noResidualPass)
  {
    for (uint8_t mrgHADIdx = 0; mrgHADIdx < geoNumMrgSATDCand; mrgHADIdx++)
    {
      uint8_t candidateIdx = geoRdModeList[mrgHADIdx];
      if (((noResidualPass != 0) && geocandHasNoResidual[candidateIdx]) || ((noResidualPass == 0) && bestIsSkip))
      {
        continue;
      }
      if ((m_pcEncCfg->m_Geo > 1) && mrgHADIdx && !bestCS->getCU(pm.chType, pm.treeType)->geo)
      {
        continue;
      }
      CodingUnit &cu = tempCS->addCU(tempCS->area, pm.chType);
      pm.setCUData(cu);
      cu.predMode         = MODE_INTER;
      cu.slice            = tempCS->slice;
      cu.tileIdx          = 0;
      cu.qp               = encTestMode.qp;
      cu.affine           = false;
      cu.mtsFlag          = false;
      cu.BcwIdx           = BCW_DEFAULT;
      cu.geo              = true;
      cu.imv              = 0;
      cu.mmvdSkip         = false;
      cu.skip             = false;
      cu.mipFlag          = false;
      cu.bdpcmMode        = 0;
      PredictionUnit &pu  = tempCS->addPU(cu, pm.chType, &cu);
      pu.mergeFlag        = true;
      pu.regularMergeFlag = false;
      pu.geoSplitDir      = comboList.list[candidateIdx].splitDir;
      pu.geoMergeIdx0     = comboList.list[candidateIdx].mergeIdx0;
      pu.geoMergeIdx1     = comboList.list[candidateIdx].mergeIdx1;
      pu.mmvdMergeFlag    = false;
      pu.mmvdMergeIdx     = MAX_UINT;

      PU::spanGeoMotionInfo(pu, mergeCtx, pu.geoSplitDir, pu.geoMergeIdx0, pu.geoMergeIdx1);
      tempCS->getPredBuf().copyFrom(geoCombinations[candidateIdx]);

      xEncodeInterResidual(tempCS, bestCS, pm, encTestMode, noResidualPass,
                           (noResidualPass == 0 ? &geocandHasNoResidual[candidateIdx] : NULL));

      if (m_pcEncCfg->m_useFastDecisionForMerge && !bestIsSkip)
      {
        bestIsSkip = bestCS->getCU(pm.chType, pm.treeType)->rootCbf == 0;
      }
      tempCS->initStructData(encTestMode.qp);
    }
  }
}

void EncCu::xCheckRDCostInter( CodingStructure *&tempCS, CodingStructure *&bestCS, Partitioner &partitioner, const EncTestMode& encTestMode )
{
  PROFILER_SCOPE_AND_STAGE_EXT( 1, g_timeProfiler, P_INTER_MVD_SEARCH, tempCS, partitioner.chType );
  tempCS->initStructData( encTestMode.qp );

  CodingUnit &cu      = tempCS->addCU( tempCS->area, partitioner.chType );

  partitioner.setCUData( cu );
  cu.slice            = tempCS->slice;
  cu.tileIdx          = 0;
  cu.skip             = false;
  cu.mmvdSkip         = false;
  cu.predMode         = MODE_INTER;
  cu.chromaQpAdj      = m_cuChromaQpOffsetIdxPlus1;
  cu.qp               = encTestMode.qp;
  CU::addPUs( cu );

  m_cInterSearch.predInterSearch( cu, partitioner );

  xEncodeInterResidual( tempCS, bestCS, partitioner, encTestMode, 0, 0, NULL );

  tempCS->initStructData(encTestMode.qp);
  STAT_COUNT_CU_MODES( partitioner.chType == CH_L, g_cuCounters1D[CU_MODES_TESTED][0][!tempCS->slice->isIntra() + tempCS->slice->depth] );
  STAT_COUNT_CU_MODES( partitioner.chType == CH_L && !tempCS->slice->isIntra(), g_cuCounters2D[CU_MODES_TESTED][Log2( tempCS->area.lheight() )][Log2( tempCS->area.lwidth() )] );
}

void EncCu::xCheckRDCostInterIMV(CodingStructure *&tempCS, CodingStructure *&bestCS, Partitioner &partitioner, const EncTestMode& encTestMode)
{
  PROFILER_SCOPE_AND_STAGE_EXT( 1, g_timeProfiler, P_INTER_MVD_SEARCH_IMV, tempCS, partitioner.chType );
  bool Test_AMVR = m_pcEncCfg->m_AMVRspeed ? true: false;
  if (m_pcEncCfg->m_AMVRspeed > 2 && m_pcEncCfg->m_AMVRspeed < 5 && bestCS->getCU(partitioner.chType, partitioner.treeType)->skip)
  {
    Test_AMVR = false;
  }
  else if (m_pcEncCfg->m_AMVRspeed > 4 && bestCS->getCU(partitioner.chType, partitioner.treeType)->pu->mergeFlag)
  {
    Test_AMVR = false;
  }
  bool Do_Limit = (m_pcEncCfg->m_AMVRspeed == 4 || m_pcEncCfg->m_AMVRspeed == 6) ? true : false;
  bool Do_OnceRes = (m_pcEncCfg->m_AMVRspeed == 7) ? true : false;

  if( Test_AMVR )
  {
    double Fpel_cost    = m_pcEncCfg->m_AMVRspeed == 1 ? MAX_DOUBLE*0.5 : MAX_DOUBLE;
    double costCurStart = m_pcEncCfg->m_AMVRspeed == 1 ? m_modeCtrl.comprCUCtx->bestCostNoImv : bestCS->cost;
    double costCur      = MAX_DOUBLE;
    double bestCostIMV  = MAX_DOUBLE;

    const unsigned wIdx = Log2(partitioner.currArea().lwidth());
    const unsigned hIdx = Log2(partitioner.currArea().lheight());

    if (Do_OnceRes)
    {
      costCurStart = xCalcDistortion(bestCS, partitioner.chType, bestCS->sps->bitDepths[CH_L], 0);
      Fpel_cost = costCurStart;
      tempCS->initSubStructure(*m_pTempCS2[wIdx][hIdx], partitioner.chType, partitioner.currArea(), false);
    }

    CodingStructure *tempCSbest = m_pTempCS2[wIdx][hIdx];


    for (int i = 1; i <= IMV_HPEL; i++)
    {
      if (i > IMV_FPEL)
      {
        bool nextimv = false;
        double stopCost = i == IMV_HPEL ? 1.25 : 1.06;
        if (Fpel_cost > stopCost * costCurStart)
        {
          nextimv = true;
        }
        if ( m_pcEncCfg->m_AMVRspeed == 1 )
        {
          costCurStart = bestCS->cost;
        }
        if (nextimv)
        {
          continue;
        }
      }

      bool Do_Search = Do_OnceRes ? false : true;

      if (Do_Limit)
      {
        Do_Search = i == IMV_FPEL ? true : false;

        if (i == IMV_HPEL)
        {
          if (bestCS->slice->TLayer > 3)
          {
            continue;
          }
          if (bestCS->getCU(partitioner.chType, partitioner.treeType)->imv != 0)
          {
            Do_Search = true; //do_est
          }
        }
      }
      tempCS->initStructData(encTestMode.qp);

      if (!Do_Search)
      {
        tempCS->copyStructure(*bestCS, partitioner.chType, TREE_D);
      }
      tempCS->dist = 0;
      tempCS->fracBits = 0;
      tempCS->cost = MAX_DOUBLE;
      CodingUnit &cu = (Do_Search) ? tempCS->addCU(tempCS->area, partitioner.chType) : *tempCS->getCU(partitioner.chType, partitioner.treeType);
      if (Do_Search)
      {
        partitioner.setCUData(cu);
        cu.slice = tempCS->slice;
        cu.tileIdx = 0;
        cu.skip = false;
        cu.mmvdSkip = false;
        cu.predMode = MODE_INTER;
        cu.chromaQpAdj = m_cuChromaQpOffsetIdxPlus1;
        cu.qp = encTestMode.qp;
        CU::addPUs(cu);

        cu.imv = i;

        m_cInterSearch.predInterSearch(cu, partitioner);
        if (!CU::hasSubCUNonZeroMVd(cu))
        {
          continue;
        }
      }
      else
      {
        cu.smvdMode = 0;
        cu.affine = false;
        cu.imv = i ;
        CU::resetMVDandMV2Int(cu);
        if (!CU::hasSubCUNonZeroMVd(cu))
        {
          continue;
        }
        cu.pu->mvRefine = true;
        m_cInterSearch.motionCompensation(*cu.pu, tempCS->getPredBuf() );
        cu.pu->mvRefine = false;
      }

      if( Do_OnceRes )
      {
        costCur = xCalcDistortion(tempCS, partitioner.chType, tempCS->sps->bitDepths[CH_L], cu.imv );
        if (costCur < bestCostIMV)
        {
          bestCostIMV = costCur;
          tempCSbest->getPredBuf().copyFrom(tempCS->getPredBuf());
          tempCSbest->clearCUs();
          tempCSbest->clearPUs();
          tempCSbest->clearTUs();
          tempCSbest->copyStructure(*tempCS, partitioner.chType, TREE_D);
        }
        if (i > IMV_FPEL)
        {
          costCurStart = costCurStart > costCur ? costCur : costCurStart;
        }
      }
      else
      {
        xEncodeInterResidual(tempCS, bestCS, partitioner, encTestMode, 0, 0, NULL);
        costCur = tempCS->cost;

        if (i > IMV_FPEL)
        {
          costCurStart = bestCS->cost;
        }
      }

      if (i == IMV_FPEL)
      {
         Fpel_cost = costCur;
      }
    }

    if (Do_OnceRes && (bestCostIMV != MAX_DOUBLE))
    {
      CodingStructure* CSCandBest = tempCSbest;
      tempCS->initStructData(bestCS->currQP[partitioner.chType]);
      tempCS->copyStructure(*CSCandBest, partitioner.chType, TREE_D);
      tempCS->getPredBuf().copyFrom(tempCSbest->getPredBuf());
      tempCS->dist = 0;
      tempCS->fracBits = 0;
      tempCS->cost = MAX_DOUBLE;

      xEncodeInterResidual(tempCS, bestCS, partitioner, encTestMode, 0, 0, NULL);
    }

    tempCS->initStructData(encTestMode.qp);
  }
  STAT_COUNT_CU_MODES( partitioner.chType == CH_L, g_cuCounters1D[CU_MODES_TESTED][0][!tempCS->slice->isIntra() + tempCS->slice->depth] );
  STAT_COUNT_CU_MODES( partitioner.chType == CH_L && !tempCS->slice->isIntra(), g_cuCounters2D[CU_MODES_TESTED][Log2( tempCS->area.lheight() )][Log2( tempCS->area.lwidth() )] );
}

void EncCu::xCalDebCost( CodingStructure &cs, Partitioner &partitioner )
{
  PROFILER_SCOPE_AND_STAGE_EXT( 1, g_timeProfiler, P_DEBLOCK_FILTER, &cs, partitioner.chType );
  if ( cs.slice->deblockingFilterDisable )
  {
    return;
  }

  const ChromaFormat format = cs.area.chromaFormat;
  CodingUnit*            cu = cs.getCU(partitioner.chType, partitioner.treeType);
  const Position    lumaPos = cu->Y().valid() ? cu->Y().pos() : recalcPosition( format, cu->chType, CH_L, cu->blocks[cu->chType].pos() );
  bool    topEdgeAvai = lumaPos.y > 0 && ((lumaPos.y % 4) == 0);
  bool   leftEdgeAvai = lumaPos.x > 0 && ((lumaPos.x % 4) == 0);

  if( ! ( topEdgeAvai || leftEdgeAvai ))
  {
    return;
  }

  ComponentID compStr = ( cu->isSepTree() && !isLuma( partitioner.chType ) ) ? COMP_Cb : COMP_Y;
  ComponentID compEnd = ( cu->isSepTree() &&  isLuma( partitioner.chType ) ) ? COMP_Y : COMP_Cr;
  const UnitArea currCsArea = clipArea( CS::getArea( cs, cs.area, partitioner.chType, partitioner.treeType ), *cs.picture );

  PelStorage&  picDbBuf = m_dbBuffer; //th we could reduce the buffer size and do some relocate

  //deblock neighbour pixels
  const Size     lumaSize = cu->Y().valid() ? cu->Y().size() : recalcSize( format, cu->chType, CH_L, cu->blocks[cu->chType].size() );

  int verOffset = lumaPos.y > 7 ? 8 : 4;
  int horOffset = lumaPos.x > 7 ? 8 : 4;

  LoopFilter::calcFilterStrengths( *cu, true );

  if( m_pcEncCfg->m_EDO == 2 && CS::isDualITree( cs ) && isLuma( partitioner.chType ) )
  {
    m_pcLoopFilter->getMaxFilterLength( *cu, verOffset, horOffset );

    if( 0== (verOffset + horOffset) )
    {
      return;
    }

    topEdgeAvai  &= verOffset != 0;
    leftEdgeAvai &= horOffset != 0;
  }

  const UnitArea  areaTop  = UnitArea( format, Area( lumaPos.x,             lumaPos.y - verOffset, lumaSize.width, verOffset       ) );
  const UnitArea  areaLeft = UnitArea( format, Area( lumaPos.x - horOffset, lumaPos.y,             horOffset,      lumaSize.height ) );

  for ( int compIdx = compStr; compIdx <= compEnd; compIdx++ )
  {
    ComponentID compId = (ComponentID)compIdx;

    //Copy current CU's reco to Deblock Pic Buffer
    const ReshapeData& reshapeData = cs.picture->reshapeData;
    const CompArea&  compArea = currCsArea.block( compId );
    PelBuf dbReco = picDbBuf.getBuf( compArea );
    if (cs.slice->lmcsEnabled && isLuma(compId) )
    {
      if ((!cs.sps->LFNST)&& (!cs.sps->MTS) && reshapeData.getCTUFlag())
      {
        PelBuf rspReco = cs.getRspRecoBuf();
        dbReco.copyFrom( rspReco );
      }
      else
      {
        PelBuf reco = cs.getRecoBuf( compId );
        dbReco.rspSignal( reco, reshapeData.getInvLUT() );
      }
    }
    else
    {
      PelBuf reco = cs.getRecoBuf( compId );
      dbReco.copyFrom( reco );
    }
    //left neighbour
    if ( leftEdgeAvai )
    {
      const CompArea&  compArea = areaLeft.block(compId);
      PelBuf dbReco = picDbBuf.getBuf( compArea );
      if (cs.slice->lmcsEnabled && isLuma(compId))
      {
        dbReco.rspSignal( cs.picture->getRecoBuf( compArea ), reshapeData.getInvLUT() );
      }
      else
      {
        dbReco.copyFrom( cs.picture->getRecoBuf( compArea ) );
      }
    }
    //top neighbour
    if ( topEdgeAvai )
    {
      const CompArea&  compArea = areaTop.block( compId );
      PelBuf dbReco = picDbBuf.getBuf( compArea );
      if (cs.slice->lmcsEnabled && isLuma(compId))
      {
        dbReco.rspSignal( cs.picture->getRecoBuf( compArea ), reshapeData.getInvLUT() );
      }
      else
      {
        dbReco.copyFrom( cs.picture->getRecoBuf( compArea ) );
      }
    }
  }

  ChannelType dbChType = cu->isSepTree() ? partitioner.chType : MAX_NUM_CH;

  CHECK( cu->isSepTree() && !cu->Y().valid() && partitioner.chType == CH_L, "xxx" );

  //deblock
  if( leftEdgeAvai )
  {
    m_pcLoopFilter->loopFilterCu( *cu, dbChType, EDGE_VER, m_dbBuffer );
  }

  if( topEdgeAvai )
  {
    m_pcLoopFilter->loopFilterCu( *cu, dbChType, EDGE_HOR, m_dbBuffer );
  }

  //calculate difference between DB_before_SSE and DB_after_SSE for neighbouring CUs
  Distortion distBeforeDb = 0, distAfterDb = 0, distCur = 0;
  for (int compIdx = compStr; compIdx <= compEnd; compIdx++)
  {
    ComponentID compId = (ComponentID)compIdx;
    {
      const CompArea&  compArea = currCsArea.block( compId );
      CPelBuf reco = picDbBuf.getBuf( compArea );
      CPelBuf org  = cs.getOrgBuf( compId );
      distCur += xGetDistortionDb( cs, org, reco, compArea, false );
    }

    if ( leftEdgeAvai )
    {
      const CompArea&  compArea = areaLeft.block( compId );
      CPelBuf org    = cs.picture->getOrigBuf( compArea );
      if ( cs.picture->getFilteredOrigBuffer().valid() )
      {
        org = cs.picture->getRspOrigBuf( compArea );
      }
      CPelBuf reco   = cs.picture->getRecoBuf( compArea );
      CPelBuf recoDb = picDbBuf.getBuf( compArea );
      distBeforeDb  += xGetDistortionDb( cs, org, reco,   compArea, true );
      distAfterDb   += xGetDistortionDb( cs, org, recoDb, compArea, false  );
    }

    if ( topEdgeAvai )
    {
      const CompArea&  compArea = areaTop.block( compId );
      CPelBuf org    = cs.picture->getOrigBuf( compArea );
      if ( cs.picture->getFilteredOrigBuffer().valid() )
      {
        org = cs.picture->getRspOrigBuf( compArea );
      }
      CPelBuf reco   = cs.picture->getRecoBuf( compArea );
      CPelBuf recoDb = picDbBuf.getBuf( compArea );
      distBeforeDb  += xGetDistortionDb( cs, org, reco,   compArea, true );
      distAfterDb   += xGetDistortionDb( cs, org, recoDb, compArea, false  );
    }
  }

  //updated cost
  int64_t distTmp = distCur - cs.dist + distAfterDb - distBeforeDb;
  cs.costDbOffset = distTmp < 0 ? -m_cRdCost.calcRdCost( 0, -distTmp ) : m_cRdCost.calcRdCost( 0, distTmp );
}

Distortion EncCu::xGetDistortionDb(CodingStructure &cs, CPelBuf& org, CPelBuf& reco, const CompArea& compArea, bool beforeDb)
{
  Distortion dist;
  const ReshapeData& reshapeData = cs.picture->reshapeData;
  const ComponentID compID = compArea.compID;
  if( (cs.slice->lmcsEnabled && reshapeData.getCTUFlag()) || m_pcEncCfg->m_lumaLevelToDeltaQPEnabled)
  {
    if ( compID == COMP_Y && !m_pcEncCfg->m_lumaLevelToDeltaQPEnabled)
    {
      CPelBuf tmpReco;
      if( beforeDb )
      {
        PelBuf tmpLmcs = m_aTmpStorageLCU[0].getCompactBuf( compArea );
        tmpLmcs.rspSignal( reco, reshapeData.getInvLUT() );
        tmpReco = tmpLmcs;
      }
      else
      {
        tmpReco = reco;
      }
      dist = m_cRdCost.getDistPart( org, tmpReco, cs.sps->bitDepths[CH_L], compID, DF_SSE_WTD, &org );
    }
    else if( m_pcEncCfg->m_EDO == 2)
    {
      // use the correct luma area to scale chroma
      const int csx = getComponentScaleX( compID, cs.area.chromaFormat );
      const int csy = getComponentScaleY( compID, cs.area.chromaFormat );
      CompArea lumaArea = CompArea( COMP_Y, cs.area.chromaFormat, Area( compArea.x << csx, compArea.y << csy, compArea.width << csx, compArea.height << csy), true);
      CPelBuf orgLuma = cs.picture->getFilteredOrigBuffer().valid() ? cs.picture->getRspOrigBuf( lumaArea ): cs.picture->getOrigBuf( lumaArea );
      dist = m_cRdCost.getDistPart( org, reco, cs.sps->bitDepths[toChannelType( compID )], compID, DF_SSE_WTD, &orgLuma );
    }
    else
    {
      CPelBuf orgLuma = cs.picture->getFilteredOrigBuffer().valid() ? cs.picture->getRspOrigBuf( cs.area.blocks[COMP_Y] ): cs.picture->getOrigBuf( cs.area.blocks[COMP_Y] );
      dist = m_cRdCost.getDistPart( org, reco, cs.sps->bitDepths[toChannelType( compID )], compID, DF_SSE_WTD, &orgLuma );
    }
    return dist;
  }

  if ( cs.slice->lmcsEnabled && cs.slice->isIntra() && compID == COMP_Y && !beforeDb ) //intra slice
  {
    PelBuf tmpLmcs = m_aTmpStorageLCU[0].getCompactBuf( compArea );
    tmpLmcs.rspSignal( reco, reshapeData.getFwdLUT() );
    dist = m_cRdCost.getDistPart( org, tmpLmcs, cs.sps->bitDepths[CH_L], compID, DF_SSE );
    return dist;
  }
  dist = m_cRdCost.getDistPart(org, reco, cs.sps->bitDepths[toChannelType(compID)], compID, DF_SSE);
  return dist;
}

bool checkValidMvs(CodingUnit* cu)
{
  PredictionUnit& pu = *cu->pu;

  // clang-format off
  const int affineShiftTab[3] =
  {
    MV_PRECISION_INTERNAL - MV_PRECISION_QUARTER,
    MV_PRECISION_INTERNAL - MV_PRECISION_SIXTEENTH,
    MV_PRECISION_INTERNAL - MV_PRECISION_INT
  };

  const int normalShiftTab[NUM_IMV_MODES] =
  {
    MV_PRECISION_INTERNAL - MV_PRECISION_QUARTER,
    MV_PRECISION_INTERNAL - MV_PRECISION_INT,
    MV_PRECISION_INTERNAL - MV_PRECISION_4PEL,
    MV_PRECISION_INTERNAL - MV_PRECISION_HALF,
  };
  // clang-format on

  int mvShift;

  for (int refList = 0; refList < NUM_REF_PIC_LIST_01; refList++)
  {
    if (pu.refIdx[refList] >= 0)
    {
      if (!cu->affine)
      {
        mvShift = normalShiftTab[cu->imv];
        Mv signaledmvd(pu.mvd[refList].hor >> mvShift, pu.mvd[refList].ver >> mvShift);
        if (!((signaledmvd.hor >= MVD_MIN) && (signaledmvd.hor <= MVD_MAX)) || !((signaledmvd.ver >= MVD_MIN) && (signaledmvd.ver <= MVD_MAX)))
          return false;
      }
      else
      {
        for (int ctrlP = 1 + (cu->affineType == AFFINEMODEL_6PARAM); ctrlP >= 0; ctrlP--)
        {
          mvShift = affineShiftTab[cu->imv];
          Mv signaledmvd(pu.mvdAffi[refList][ctrlP].hor >> mvShift, pu.mvdAffi[refList][ctrlP].ver >> mvShift);
          if (!((signaledmvd.hor >= MVD_MIN) && (signaledmvd.hor <= MVD_MAX)) || !((signaledmvd.ver >= MVD_MIN) && (signaledmvd.ver <= MVD_MAX)))
            return false;;
        }
      }
    }
  }
  // avoid MV exceeding 18-bit dynamic range
  const int maxMv = 1 << 17;
  if (!cu->affine && !pu.mergeFlag)
  {
    if ((pu.refIdx[0] >= 0 && (pu.mv[0].getAbsHor() >= maxMv || pu.mv[0].getAbsVer() >= maxMv))
      || (pu.refIdx[1] >= 0 && (pu.mv[1].getAbsHor() >= maxMv || pu.mv[1].getAbsVer() >= maxMv)))
    {
      return false;
    }
  }
  if (cu->affine && !pu.mergeFlag)
  {
    for (int refList = 0; refList < NUM_REF_PIC_LIST_01; refList++)
    {
      if (pu.refIdx[refList] >= 0)
      {
        for (int ctrlP = 1 + (cu->affineType == AFFINEMODEL_6PARAM); ctrlP >= 0; ctrlP--)
        {
          if (pu.mvAffi[refList][ctrlP].getAbsHor() >= maxMv || pu.mvAffi[refList][ctrlP].getAbsVer() >= maxMv)
          {
            return false;
          }
        }
      }
    }
  }
  return true;
}


void EncCu::xEncodeInterResidual( CodingStructure *&tempCS, CodingStructure *&bestCS, Partitioner &partitioner, const EncTestMode& encTestMode, int residualPass, bool* bestHasNonResi, double* equBcwCost )
{
  if( residualPass == 1 && encTestMode.lossless )
  {
    return;
  }

  CodingUnit*            cu        = tempCS->getCU( partitioner.chType, partitioner.treeType );
  double   bestCostInternal        = MAX_DOUBLE;

  if( ! checkValidMvs(cu))
    return;

  double  currBestCost = MAX_DOUBLE;

  // For SBT
  double     bestCost          = bestCS->cost;
  double     bestCostBegin     = bestCS->cost;
  const CodingUnit* prevBestCU = bestCS->getCU( partitioner.chType, partitioner.treeType );
  uint8_t    prevBestSbt       = ( prevBestCU == nullptr ) ? 0 : prevBestCU->sbtInfo;
  Distortion sbtOffDist        = 0;
  bool       sbtOffRootCbf     = 0;
  double     sbtOffCost        = MAX_DOUBLE;
  uint8_t    currBestSbt       = 0;
  uint8_t    histBestSbt       = MAX_UCHAR;
  Distortion curPuSse          = MAX_DISTORTION;
  uint8_t    numRDOTried       = 0;
  bool       doPreAnalyzeResi  = false;
  const bool mtsAllowed        = tempCS->sps->MTSInter && partitioner.currArea().lwidth() <= MTS_INTER_MAX_CU_SIZE && partitioner.currArea().lheight() <= MTS_INTER_MAX_CU_SIZE;

  uint8_t sbtAllowed = cu->checkAllowedSbt();
  if( tempCS->pps->picWidthInLumaSamples < (uint32_t)SBT_FAST64_WIDTH_THRESHOLD || m_pcEncCfg->m_SBT>1)
  {
    sbtAllowed = ((cu->lwidth() > 32 || cu->lheight() > 32)) ? 0 : sbtAllowed;
  }

  if( sbtAllowed )
  {
    //SBT resolution-dependent fast algorithm: not try size-64 SBT in RDO for low-resolution sequences (now resolution below HD)
    doPreAnalyzeResi = ( sbtAllowed || mtsAllowed ) && residualPass == 0;
    m_cInterSearch.getBestSbt( tempCS, cu, histBestSbt, curPuSse, sbtAllowed, doPreAnalyzeResi, mtsAllowed );
  }

  cu->skip    = false;
  cu->sbtInfo = 0;

  const bool skipResidual = residualPass == 1;
  if( skipResidual || histBestSbt == MAX_UCHAR || !CU::isSbtMode( histBestSbt ) )
  {
    m_cInterSearch.encodeResAndCalcRdInterCU( *tempCS, partitioner, skipResidual );
    xEncodeDontSplit( *tempCS, partitioner );
    xCheckDQP( *tempCS, partitioner );

    if( NULL != bestHasNonResi && (bestCostInternal > tempCS->cost) )
    {
      bestCostInternal = tempCS->cost;
      if (!(tempCS->getPU(partitioner.chType)->ciip))
      *bestHasNonResi  = !cu->rootCbf;
    }

    if (cu->rootCbf == false)
    {
      if (tempCS->getPU(partitioner.chType)->ciip)
      {
        tempCS->cost = MAX_DOUBLE;
        tempCS->costDbOffset = 0;
        return;
      }
    }
    currBestCost = tempCS->cost;
    if( sbtAllowed )
    {
      sbtOffCost    = tempCS->cost;
      sbtOffDist    = tempCS->dist;
      sbtOffRootCbf = cu->rootCbf;
      currBestSbt   = cu->firstTU->mtsIdx[COMP_Y] > MTS_SKIP ? SBT_OFF_MTS : SBT_OFF_DCT;
      numRDOTried  += mtsAllowed ? 2 : 1;
    }

    DTRACE_MODE_COST( *tempCS, m_cRdCost.getLambda( true ) );
    xCheckBestMode( tempCS, bestCS, partitioner, encTestMode );

    STAT_COUNT_CU_MODES( partitioner.chType == CH_L, g_cuCounters1D[CU_RD_TESTS][0][!tempCS->slice->isIntra() + tempCS->slice->depth] );
    STAT_COUNT_CU_MODES( partitioner.chType == CH_L && !tempCS->slice->isIntra(), g_cuCounters2D[CU_RD_TESTS][Log2( tempCS->area.lheight() )][Log2( tempCS->area.lwidth() )] );
  }

  if( sbtAllowed && (m_pcEncCfg->m_SBT == 1 || sbtOffRootCbf))
  {
    bool swapped = false; // avoid unwanted data copy
    uint8_t numSbtRdo   = CU::numSbtModeRdo( sbtAllowed );
    //early termination if all SBT modes are not allowed
    //normative
    if( !sbtAllowed || skipResidual )
    {
      numSbtRdo = 0;
    }
    //fast algorithm
    if( ( histBestSbt != MAX_UCHAR && !CU::isSbtMode( histBestSbt ) ) || m_cInterSearch.getSkipSbtAll() )
    {
      numSbtRdo = 0;
    }
    if( bestCost != MAX_DOUBLE && sbtOffCost != MAX_DOUBLE )
    {
      double th = 1.07;
      if( !( prevBestSbt == 0 || m_sbtCostSave[0] == MAX_DOUBLE ) )
      {
        assert( m_sbtCostSave[1] <= m_sbtCostSave[0] );
        th *= ( m_sbtCostSave[0] / m_sbtCostSave[1] );
      }
      if( sbtOffCost > bestCost * th )
      {
        numSbtRdo = 0;
      }
    }
    if( !sbtOffRootCbf && sbtOffCost != MAX_DOUBLE )
    {
      double th = Clip3( 0.05, 0.55, ( 27 - cu->qp ) * 0.02 + 0.35 );
      if( sbtOffCost < m_cRdCost.calcRdCost( ( cu->lwidth() * cu->lheight() ) << SCALE_BITS, 0 ) * th )
      {
        numSbtRdo = 0;
      }
    }

    if( histBestSbt != MAX_UCHAR && numSbtRdo != 0 )
    {
      numSbtRdo = 1;
      m_cInterSearch.initSbtRdoOrder( CU::getSbtMode( CU::getSbtIdx( histBestSbt ), CU::getSbtPos( histBestSbt ) ) );
    }

    for( int sbtModeIdx = 0; sbtModeIdx < numSbtRdo; sbtModeIdx++ )
    {
      uint8_t sbtMode = m_cInterSearch.getSbtRdoOrder( sbtModeIdx );
      uint8_t sbtIdx = CU::getSbtIdxFromSbtMode( sbtMode );
      uint8_t sbtPos = CU::getSbtPosFromSbtMode( sbtMode );

      //fast algorithm (early skip, save & load)
      if( histBestSbt == MAX_UCHAR )
      {
        uint8_t skipCode = m_cInterSearch.skipSbtByRDCost( cu->lwidth(), cu->lheight(), cu->mtDepth, sbtIdx, sbtPos, bestCS->cost, sbtOffDist, sbtOffCost, sbtOffRootCbf );
        if( skipCode != MAX_UCHAR )
        {
          continue;
        }

        if( sbtModeIdx > 0 )
        {
          uint8_t prevSbtMode = m_cInterSearch.getSbtRdoOrder( sbtModeIdx - 1 );
          //make sure the prevSbtMode is the same size as the current SBT mode (otherwise the estimated dist may not be comparable)
          if( CU::isSameSbtSize( prevSbtMode, sbtMode ) )
          {
            Distortion currEstDist = m_cInterSearch.getEstDistSbt( sbtMode );
            Distortion prevEstDist = m_cInterSearch.getEstDistSbt( prevSbtMode );
            if( currEstDist > prevEstDist * 1.15 )
            {
              continue;
            }
          }
        }
      }

      //init tempCS and TU
      if( bestCost == bestCS->cost ) //The first EMT pass didn't become the bestCS, so we clear the TUs generated
      {
        tempCS->clearTUs();
      }
      else if( !swapped )
      {
        tempCS->initStructData( encTestMode.qp );
        tempCS->copyStructure( *bestCS, partitioner.chType, partitioner.treeType );
        tempCS->getPredBuf().copyFrom( bestCS->getPredBuf() );
        bestCost = bestCS->cost;
        cu = tempCS->getCU( partitioner.chType, partitioner.treeType );
        swapped = true;
      }
      else
      {
        tempCS->clearTUs();
        bestCost = bestCS->cost;
        cu = tempCS->getCU( partitioner.chType, partitioner.treeType );
      }

      //we need to restart the distortion for the new tempCS, the bit count and the cost
      tempCS->dist     = 0;
      tempCS->fracBits = 0;
      tempCS->cost     = MAX_DOUBLE;
      cu->skip         = false;


      //set SBT info
      cu->sbtInfo = (sbtPos << 4) + sbtIdx;

      //try residual coding
      m_cInterSearch.encodeResAndCalcRdInterCU( *tempCS, partitioner, skipResidual );
      numRDOTried++;

      xEncodeDontSplit( *tempCS, partitioner );
      xCheckDQP( *tempCS, partitioner );

      if( NULL != bestHasNonResi && ( bestCostInternal > tempCS->cost ) )
      {
        bestCostInternal = tempCS->cost;
        if( !( tempCS->getPU( partitioner.chType )->ciip ) )
          *bestHasNonResi = !cu->rootCbf;
      }

      if( tempCS->cost < currBestCost )
      {
        currBestSbt = cu->sbtInfo;
        currBestCost = tempCS->cost;
      }
      else if( m_pcEncCfg->m_SBT > 2 )
      {
        sbtModeIdx = numSbtRdo;
      }

      DTRACE_MODE_COST( *tempCS, m_cRdCost.getLambda( true ) );
      xCheckBestMode( tempCS, bestCS, partitioner, encTestMode );
      STAT_COUNT_CU_MODES( partitioner.chType == CH_L, g_cuCounters1D[CU_RD_TESTS][0][!tempCS->slice->isIntra() + tempCS->slice->depth] );
      STAT_COUNT_CU_MODES( partitioner.chType == CH_L && !tempCS->slice->isIntra(), g_cuCounters2D[CU_RD_TESTS][Log2( tempCS->area.lheight() )][Log2( tempCS->area.lwidth() )] );
    }

    if( bestCostBegin != bestCS->cost )
    {
      m_sbtCostSave[0] = sbtOffCost;
      m_sbtCostSave[1] = currBestCost;
    }

    if( histBestSbt == MAX_UCHAR && doPreAnalyzeResi && numRDOTried > 1 )
    {
      auto slsSbt = dynamic_cast<SaveLoadEncInfoSbt*>( &m_modeCtrl );
      int slShift = 4 + std::min( Log2( cu->lwidth() ) + Log2( cu->lheight() ), 9 );
      slsSbt->saveBestSbt( cu->cs->area, (uint32_t)( curPuSse >> slShift ), currBestSbt );
    }
  }

  tempCS->cost = currBestCost;
}

void EncCu::xEncodeDontSplit( CodingStructure &cs, Partitioner &partitioner )
{
  m_CABACEstimator->resetBits();

  m_CABACEstimator->split_cu_mode( CU_DONT_SPLIT, cs, partitioner );
  if( partitioner.treeType == TREE_C )
    CHECK( m_CABACEstimator->getEstFracBits() != 0, "must be 0 bit" );

  cs.fracBits += m_CABACEstimator->getEstFracBits(); // split bits
  cs.cost      = m_cRdCost.calcRdCost( cs.fracBits, cs.dist );
}

void EncCu::xReuseCachedResult( CodingStructure *&tempCS, CodingStructure *&bestCS, Partitioner &partitioner )
{
  EncTestMode cachedMode;

  if( ! m_modeCtrl.setCsFrom( *tempCS, cachedMode, partitioner ) )
  {
    THROW( "Should never happen!" );
  }

  CodingUnit& cu = *tempCS->cus.front();
  partitioner.setCUData( cu );

  if( CU::isIntra( cu ) )
  {
    xReconIntraQT( cu );
  }
  else
  {
    xDeriveCUMV( cu );
    xReconInter( cu );
  }

  m_CABACEstimator->getCtx() = m_CurrCtx->start;
  m_CABACEstimator->resetBits();

  CUCtx cuCtx;
  cuCtx.isDQPCoded = true;
  cuCtx.isChromaQpAdjCoded = true;
  m_CABACEstimator->coding_unit( cu, partitioner, cuCtx );

  tempCS->fracBits = m_CABACEstimator->getEstFracBits();
  tempCS->cost     = m_cRdCost.calcRdCost( tempCS->fracBits, tempCS->dist );

  xEncodeDontSplit( *tempCS,         partitioner );
  xCheckDQP       ( *tempCS,         partitioner );
  xCheckBestMode  (  tempCS, bestCS, partitioner, cachedMode, m_pcEncCfg->m_EDO );
}

void EncCu::xCheckRDCostAffineMerge(CodingStructure *&tempCS, CodingStructure *&bestCS, Partitioner &partitioner, const EncTestMode& encTestMode)
{
  PROFILER_SCOPE_AND_STAGE_EXT( 1, g_timeProfiler, P_INTER_MRG_AFFINE, tempCS, partitioner.chType );
  if (bestCS->area.lumaSize().width < 8 || bestCS->area.lumaSize().height < 8 || bestCS->sps->maxNumAffineMergeCand == 0 )
  {
    return;
  }

  const Slice &slice = *tempCS->slice;

  CHECK(slice.sliceType == I_SLICE, "Affine Merge modes not available for I-slices");

  tempCS->initStructData(encTestMode.qp);

  AffineMergeCtx affineMergeCtx;
  const SPS &sps = *tempCS->sps;

  m_AFFBestSATDCost = MAX_DOUBLE;

  MergeCtx mrgCtx;
  if (sps.SbtMvp)
  {
    Size bufSize = g_miScaling.scale(tempCS->area.lumaSize());
    mrgCtx.subPuMvpMiBuf = MotionBuf(m_subPuMiBuf, bufSize);
    affineMergeCtx.mrgCtx = &mrgCtx;
  }
  m_SortedPelUnitBufs.reset();

  {
    // first get merge candidates
    CodingUnit cu(tempCS->area);
    cu.cs       = tempCS;
    cu.predMode = MODE_INTER;
    cu.slice    = tempCS->slice;
    cu.tileIdx  = 0;
    cu.mmvdSkip = false;

    PredictionUnit pu(tempCS->area);
    pu.cu = &cu;
    pu.cs = tempCS;
    pu.regularMergeFlag = false;
    PU::getAffineMergeCand(pu, affineMergeCtx);

    if (affineMergeCtx.numValidMergeCand <= 0)
    {
      return;
    }
  }

  bool candHasNoResidual[AFFINE_MRG_MAX_NUM_CANDS];
  for (uint32_t ui = 0; ui < affineMergeCtx.numValidMergeCand; ui++)
  {
    candHasNoResidual[ui] = false;
  }

  bool                                        bestIsSkip = false;
  uint32_t                                    uiNumMrgSATDCand = affineMergeCtx.numValidMergeCand;
  static_vector<uint32_t, AFFINE_MRG_MAX_NUM_CANDS>  RdModeList;

  for (uint32_t i = 0; i < AFFINE_MRG_MAX_NUM_CANDS; i++)
  {
    RdModeList.push_back(i);
  }

  if (m_pcEncCfg->m_useFastMrg)
  {
    uiNumMrgSATDCand = std::min(NUM_AFF_MRG_SATD_CAND, affineMergeCtx.numValidMergeCand);
    bestIsSkip = m_modeCtrl.getBlkInfo(tempCS->area).isSkip;

    static_vector<double, AFFINE_MRG_MAX_NUM_CANDS> candCostList;

    // 1. Pass: get SATD-cost for selected candidates and reduce their count
    if (!bestIsSkip)
    {
      const UnitArea localUnitArea(tempCS->area.chromaFormat, Area(0, 0, tempCS->area.Y().width, tempCS->area.Y().height));
      m_SortedPelUnitBufs.prepare(localUnitArea, uiNumMrgSATDCand);
      RdModeList.clear();
      const double sqrtLambdaForFirstPass = m_cRdCost.getMotionLambda();

      CodingUnit &cu = tempCS->addCU(tempCS->area, partitioner.chType);

      partitioner.setCUData(cu);
      cu.slice = tempCS->slice;
      cu.tileIdx = 0;
      cu.skip = false;
      cu.affine = true;
      cu.predMode = MODE_INTER;
      cu.chromaQpAdj = m_cuChromaQpOffsetIdxPlus1;
      cu.qp = encTestMode.qp;

      PredictionUnit &pu = tempCS->addPU(cu, partitioner.chType, &cu);

      const DFunc dfunc = encTestMode.lossless || tempCS->slice->disableSATDForRd ? DF_SAD : DF_HAD;
      DistParam distParam = m_cRdCost.setDistParam(tempCS->getOrgBuf(COMP_Y), m_SortedPelUnitBufs.getTestBuf(COMP_Y), sps.bitDepths[CH_L], dfunc);

      bool sameMV[5] = { false };
      if (m_pcEncCfg->m_Affine > 1)
      {
        for (int m = 0; m < affineMergeCtx.numValidMergeCand; m++)
        {
          if((bestCS->slice->TLayer > 3) && (affineMergeCtx.mergeType[m] != MRG_TYPE_SUBPU_ATMVP))
          {
            sameMV[m] = m!=0;
          }
          else
          {
            if (sameMV[m + 1] == false)
            {
              for (int n = m + 1; n < affineMergeCtx.numValidMergeCand; n++)
              {
                if( (affineMergeCtx.mvFieldNeighbours[(m << 1) + 0]->mv == affineMergeCtx.mvFieldNeighbours[(n << 1) + 0]->mv)
                 && (affineMergeCtx.mvFieldNeighbours[(m << 1) + 1]->mv == affineMergeCtx.mvFieldNeighbours[(n << 1) + 1]->mv))
                {
                  sameMV[n] = true;
                }
              }
            }
          }
        }
      }

      for (uint32_t uiMergeCand = 0; uiMergeCand < affineMergeCtx.numValidMergeCand; uiMergeCand++)
      {
        if ((m_pcEncCfg->m_Affine > 1) && sameMV[uiMergeCand])
        {
          continue;
        }

        // set merge information
        pu.interDir = affineMergeCtx.interDirNeighbours[uiMergeCand];
        pu.mergeFlag = true;
        pu.regularMergeFlag = false;
        pu.mergeIdx = uiMergeCand;
        cu.affineType = affineMergeCtx.affineType[uiMergeCand];
        cu.BcwIdx = affineMergeCtx.BcwIdx[uiMergeCand];

        pu.mergeType = affineMergeCtx.mergeType[uiMergeCand];
        if (pu.mergeType == MRG_TYPE_SUBPU_ATMVP)
        {
          pu.refIdx[0] = affineMergeCtx.mvFieldNeighbours[(uiMergeCand << 1) + 0][0].refIdx;
          pu.refIdx[1] = affineMergeCtx.mvFieldNeighbours[(uiMergeCand << 1) + 1][0].refIdx;
          PU::spanMotionInfo(pu, mrgCtx);
        }
        else
        {
          PU::setAllAffineMvField(pu, affineMergeCtx.mvFieldNeighbours[(uiMergeCand << 1) + 0], REF_PIC_LIST_0);
          PU::setAllAffineMvField(pu, affineMergeCtx.mvFieldNeighbours[(uiMergeCand << 1) + 1], REF_PIC_LIST_1);
          PU::spanMotionInfo(pu);
        }

        distParam.cur.buf = m_SortedPelUnitBufs.getTestBuf().Y().buf;
        pu.mcControl = 2;
        m_cInterSearch.motionCompensation(pu, m_SortedPelUnitBufs.getTestBuf(), REF_PIC_LIST_X);
        pu.mcControl = 0;

        Distortion uiSad = distParam.distFunc(distParam);
        uint32_t   uiBitsCand = uiMergeCand + 1;
        if (uiMergeCand == tempCS->picHeader->maxNumAffineMergeCand - 1)
        {
          uiBitsCand--;
        }
        double cost = (double)uiSad + (double)uiBitsCand * sqrtLambdaForFirstPass;
        int insertPos = -1;
        updateCandList(uiMergeCand, cost, RdModeList, candCostList, uiNumMrgSATDCand, &insertPos);
        m_SortedPelUnitBufs.insert(insertPos, (int)RdModeList.size());

        CHECK(std::min(uiMergeCand + 1, uiNumMrgSATDCand) != RdModeList.size(), "");
      }
      double ThesholdCost = MRG_FAST_RATIO * candCostList[0];
      if (m_pcEncCfg->m_Affine > 1)
      {
        uiNumMrgSATDCand = int(uiNumMrgSATDCand) > int(candCostList.size()) ? int(candCostList.size()) : int(uiNumMrgSATDCand);
        ThesholdCost = candCostList[0] + sqrtLambdaForFirstPass;
      }

      // Try to limit number of candidates using SATD-costs
      for (uint32_t i = 1; i < uiNumMrgSATDCand; i++)
      {
        if(candCostList[i] > ThesholdCost)
        {
          uiNumMrgSATDCand = i;
          break;
        }
      }

      tempCS->initStructData(encTestMode.qp);
      m_AFFBestSATDCost = candCostList[0];
    }
    else
    {
      uiNumMrgSATDCand = affineMergeCtx.numValidMergeCand;
    }
  }

  uint32_t iteration;
  uint32_t iterationBegin = 0;
  if (encTestMode.lossless)
  {
    iteration = 1;
  }
  else
  {
    iteration = 2;
  }
  for (uint32_t uiNoResidualPass = iterationBegin; uiNoResidualPass < iteration; ++uiNoResidualPass)
  {
    for (uint32_t uiMrgHADIdx = 0; uiMrgHADIdx < uiNumMrgSATDCand; uiMrgHADIdx++)
    {
      uint32_t uiMergeCand = RdModeList[uiMrgHADIdx];

      if (((uiNoResidualPass != 0) && candHasNoResidual[uiMergeCand])
        || ((uiNoResidualPass == 0) && bestIsSkip))
      {
        continue;
      }

      // first get merge candidates
      CodingUnit &cu = tempCS->addCU(tempCS->area, partitioner.chType);

      partitioner.setCUData(cu);
      cu.slice = tempCS->slice;
      cu.tileIdx = 0;
      cu.skip = false;
      cu.affine = true;
      cu.predMode = MODE_INTER;
      cu.chromaQpAdj = m_cuChromaQpOffsetIdxPlus1;
      cu.qp = encTestMode.qp;
      PredictionUnit &pu = tempCS->addPU(cu, partitioner.chType, &cu);

      // set merge information
      pu.mergeFlag = true;
      pu.mergeIdx = uiMergeCand;
      pu.interDir = affineMergeCtx.interDirNeighbours[uiMergeCand];
      cu.affineType = affineMergeCtx.affineType[uiMergeCand];
      cu.BcwIdx = affineMergeCtx.BcwIdx[uiMergeCand];

      pu.mergeType = affineMergeCtx.mergeType[uiMergeCand];
      if (pu.mergeType == MRG_TYPE_SUBPU_ATMVP)
      {
        pu.refIdx[0] = affineMergeCtx.mvFieldNeighbours[(uiMergeCand << 1) + 0][0].refIdx;
        pu.refIdx[1] = affineMergeCtx.mvFieldNeighbours[(uiMergeCand << 1) + 1][0].refIdx;
        PU::spanMotionInfo(pu, mrgCtx);
      }
      else
      {
        PU::setAllAffineMvField(pu, affineMergeCtx.mvFieldNeighbours[(uiMergeCand << 1) + 0], REF_PIC_LIST_0);
        PU::setAllAffineMvField(pu, affineMergeCtx.mvFieldNeighbours[(uiMergeCand << 1) + 1], REF_PIC_LIST_1);

        PU::spanMotionInfo(pu);
      }

      PelUnitBuf* sortedListBuf = m_SortedPelUnitBufs.getBufFromSortedList(uiMrgHADIdx);

      if (sortedListBuf)
      {
        tempCS->getPredBuf().copyFrom(*sortedListBuf, true, false);   // Copy Luma Only
        pu.mcControl = 4;
        m_cInterSearch.motionCompensation(pu, tempCS->getPredBuf(), REF_PIC_LIST_X);
      }
      else
      {
        m_cInterSearch.motionCompensation(pu, tempCS->getPredBuf());
      }

      xEncodeInterResidual(tempCS, bestCS, partitioner, encTestMode, uiNoResidualPass, (uiNoResidualPass == 0 ? &candHasNoResidual[uiMergeCand] : NULL));

      if (m_pcEncCfg->m_useFastDecisionForMerge && !bestIsSkip)
      {
        bestIsSkip = bestCS->getCU(partitioner.chType, partitioner.treeType)->rootCbf == 0;
      }
      tempCS->initStructData(encTestMode.qp);
    }// end loop uiMrgHADIdx

    if (uiNoResidualPass == 0 && m_pcEncCfg->m_useEarlySkipDetection)
    {
      const CodingUnit     &bestCU = *bestCS->getCU(partitioner.chType, partitioner.treeType);
      const PredictionUnit &bestPU = *bestCS->getPU(partitioner.chType);

      if (bestCU.rootCbf == 0)
      {
        if (bestPU.mergeFlag)
        {
          m_modeCtrl.comprCUCtx->earlySkip = true;
        }
        else if (m_pcEncCfg->m_motionEstimationSearchMethod != MESEARCH_SELECTIVE)
        {
          int absolute_MV = 0;

          for (uint32_t uiRefListIdx = 0; uiRefListIdx < 2; uiRefListIdx++)
          {
            if (slice.numRefIdx[uiRefListIdx] > 0)
            {
              absolute_MV += bestPU.mvd[uiRefListIdx].getAbsHor() + bestPU.mvd[uiRefListIdx].getAbsVer();
            }
          }

          if (absolute_MV == 0)
          {
            m_modeCtrl.comprCUCtx->earlySkip = true;
          }
        }
      }
    }
  }
  STAT_COUNT_CU_MODES( partitioner.chType == CH_L, g_cuCounters1D[CU_MODES_TESTED][0][!tempCS->slice->isIntra() + tempCS->slice->depth] );
  STAT_COUNT_CU_MODES( partitioner.chType == CH_L && !tempCS->slice->isIntra(), g_cuCounters2D[CU_MODES_TESTED][Log2( tempCS->area.lheight() )][Log2( tempCS->area.lwidth() )] );
}

uint64_t EncCu::xCalcPuMeBits(const PredictionUnit& pu)
{
  assert(pu.mergeFlag);
  assert(!CU::isIBC(*pu.cu));
  m_CABACEstimator->resetBits();
  m_CABACEstimator->merge_flag(pu);
  if (pu.mergeFlag)
  {
    m_CABACEstimator->merge_data(pu);
  }
  return m_CABACEstimator->getEstFracBits();
}

double EncCu::xCalcDistortion(CodingStructure *&cur_CS, ChannelType chType, int BitDepth, int imv)
{
  const auto currDist1 = m_cRdCost.getDistPart(cur_CS->getOrgBuf( COMP_Y ), cur_CS->getPredBuf( COMP_Y ), BitDepth, COMP_Y, DF_HAD);
  unsigned int uiMvBits = 0;
  unsigned imvShift = imv == IMV_HPEL ? 1 : (imv << 1);
  if (cur_CS->getPU(chType)->interDir != 2)
  {
    uiMvBits += m_cRdCost.getBitsOfVectorWithPredictor(cur_CS->getPU(chType)->mvd[0].hor, cur_CS->getPU(chType)->mvd[0].ver, imvShift + MV_FRACTIONAL_BITS_DIFF);
  }
  if (cur_CS->getPU(chType)->interDir != 1)
  {
    uiMvBits += m_cRdCost.getBitsOfVectorWithPredictor(cur_CS->getPU(chType)->mvd[1].hor, cur_CS->getPU(chType)->mvd[1].ver, imvShift + MV_FRACTIONAL_BITS_DIFF);
  }
  return (double(currDist1) + (double)m_cRdCost.getCost(uiMvBits));
}

int EncCu::xCheckMMVDCand(uint32_t& mmvdMergeCand, int& bestDir, int tempNum, double& bestCostOffset, double& bestCostMerge, double bestCostList )
{
  int baseIdx = mmvdMergeCand / MMVD_MAX_REFINE_NUM;
  int CandCur = mmvdMergeCand - MMVD_MAX_REFINE_NUM*baseIdx;
  if (m_pcEncCfg->m_MMVD > 2)
  {
    if (CandCur % 4 == 0)
    {
      if ((bestCostOffset >= bestCostMerge) && (CandCur >= 4))
      {
        if (mmvdMergeCand > MMVD_MAX_REFINE_NUM)
        {
          return 2;
        }
        else
        {
          mmvdMergeCand = MMVD_MAX_REFINE_NUM;
          if (tempNum == mmvdMergeCand)
          {
            return 2;
          }
        }
      }
      //reset
      bestCostOffset = MAX_DOUBLE;
      bestCostMerge = bestCostList;
    }
  }

  if (mmvdMergeCand == MMVD_MAX_REFINE_NUM)
  {
    bestDir = 0;
  }
  if (CandCur >= 4)
  {
    if (CandCur % 4 != bestDir)
    {
      return 1;
    }
  }
  return 0;
}


} // namespace vvenc

//! \}
