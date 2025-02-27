/******************************************************************************
 *
 * Project:  GDAL
 * Purpose:  Implements Geolocation array based transformer.
 * Author:   Frank Warmerdam, warmerdam@pobox.com
 *
 ******************************************************************************
 * Copyright (c) 2006, Frank Warmerdam <warmerdam@pobox.com>
 * Copyright (c) 2007-2013, Even Rouault <even dot rouault at spatialys.com>
 * Copyright (c) 2021, CLS
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 ****************************************************************************/

#include "cpl_port.h"
#include "gdal_alg.h"
#include "gdal_alg_priv.h"

#include <climits>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>

#include <algorithm>
#include <limits>

#include "cpl_conv.h"
#include "cpl_error.h"
#include "cpl_minixml.h"
#include "cpl_string.h"
#include "cpl_vsi.h"
#include "gdal.h"
#include "gdal_priv.h"
#include "memdataset.h"

CPL_CVSID("$Id$")

CPL_C_START
CPLXMLNode *GDALSerializeGeoLocTransformer( void *pTransformArg );
void *GDALDeserializeGeoLocTransformer( CPLXMLNode *psTree );
CPL_C_END

/************************************************************************/
/* ==================================================================== */
/*                         GDALGeoLocTransformer                        */
/* ==================================================================== */
/************************************************************************/


//Constants to track down systematic shifts
const double FSHIFT = 0.5;
const double ISHIFT = 0.5;
const double OVERSAMPLE_FACTOR=1.3;

/************************************************************************/
/*                         GeoLocLoadFullData()                         */
/************************************************************************/

static bool GeoLocLoadFullData( GDALGeoLocTransformInfo *psTransform )

{
    const int nXSize_XBand = GDALGetRasterXSize( psTransform->hDS_X );
    const int nYSize_XBand = GDALGetRasterYSize( psTransform->hDS_X );
    const int nXSize_YBand = GDALGetRasterXSize( psTransform->hDS_Y );
    const int nYSize_YBand = GDALGetRasterYSize( psTransform->hDS_Y );

    // Is it a regular grid ? That is:
    // The XBAND contains the x coordinates for all lines.
    // The YBAND contains the y coordinates for all columns.
    const bool bIsRegularGrid = ( nYSize_XBand == 1 && nYSize_YBand == 1 );

    const int nXSize = nXSize_XBand;
    int nYSize = 0;
    if( bIsRegularGrid )
    {
        nYSize = nXSize_YBand;
    }
    else
    {
        nYSize = nYSize_XBand;
    }

    psTransform->nGeoLocXSize = static_cast<size_t>(nXSize);
    psTransform->nGeoLocYSize = static_cast<size_t>(nYSize);

    psTransform->padfGeoLocY = static_cast<double *>(
        VSI_MALLOC3_VERBOSE(sizeof(double), nXSize, nYSize));
    psTransform->padfGeoLocX = static_cast<double *>(
        VSI_MALLOC3_VERBOSE(sizeof(double), nXSize, nYSize));

    if( psTransform->padfGeoLocX == nullptr ||
        psTransform->padfGeoLocY == nullptr )
    {
        return false;
    }

    if( bIsRegularGrid )
    {
        // Case of regular grid.
        // The XBAND contains the x coordinates for all lines.
        // The YBAND contains the y coordinates for all columns.

        double* padfTempX = static_cast<double *>(
            VSI_MALLOC2_VERBOSE(nXSize, sizeof(double)));
        double* padfTempY = static_cast<double *>(
            VSI_MALLOC2_VERBOSE(nYSize, sizeof(double)));
        if( padfTempX == nullptr || padfTempY == nullptr )
        {
            CPLFree(padfTempX);
            CPLFree(padfTempY);
            return false;
        }

        CPLErr eErr =
            GDALRasterIO( psTransform->hBand_X, GF_Read,
                          0, 0, nXSize, 1,
                          padfTempX, nXSize, 1,
                          GDT_Float64, 0, 0 );

        for( size_t j = 0; j < static_cast<size_t>(nYSize); j++ )
        {
            memcpy( psTransform->padfGeoLocX + j * nXSize,
                    padfTempX,
                    nXSize * sizeof(double) );
        }

        if( eErr == CE_None )
        {
            eErr = GDALRasterIO( psTransform->hBand_Y, GF_Read,
                                 0, 0, nYSize, 1,
                                 padfTempY, nYSize, 1,
                                 GDT_Float64, 0, 0 );

            for( size_t j = 0; j < static_cast<size_t>(nYSize); j++ )
            {
                for( size_t i = 0; i < static_cast<size_t>(nXSize); i++ )
                {
                    psTransform->padfGeoLocY[j * nXSize + i] = padfTempY[j];
                }
            }
        }

        CPLFree(padfTempX);
        CPLFree(padfTempY);

        if( eErr != CE_None )
            return false;
    }
    else
    {
        if( GDALRasterIO( psTransform->hBand_X, GF_Read,
                          0, 0, nXSize, nYSize,
                          psTransform->padfGeoLocX, nXSize, nYSize,
                          GDT_Float64, 0, 0 ) != CE_None
            || GDALRasterIO( psTransform->hBand_Y, GF_Read,
                             0, 0, nXSize, nYSize,
                             psTransform->padfGeoLocY, nXSize, nYSize,
                             GDT_Float64, 0, 0 ) != CE_None )
            return false;
    }

    psTransform->dfNoDataX =
        GDALGetRasterNoDataValue( psTransform->hBand_X,
                                  &(psTransform->bHasNoData) );

/* -------------------------------------------------------------------- */
/*      Scan forward map for lat/long extents.                          */
/* -------------------------------------------------------------------- */
    psTransform->dfMinX = std::numeric_limits<double>::max();
    psTransform->dfMaxX = -std::numeric_limits<double>::max();
    psTransform->dfMinY = std::numeric_limits<double>::max();
    psTransform->dfMaxY = -std::numeric_limits<double>::max();

    const size_t nXYCount = psTransform->nGeoLocXSize * psTransform->nGeoLocYSize;
    for( size_t i = 0; i < nXYCount; i++ )
    {
        if( !psTransform->bHasNoData ||
            psTransform->padfGeoLocX[i] != psTransform->dfNoDataX )
        {
            if( psTransform->padfGeoLocX[i] < psTransform->dfMinX )
            {
                psTransform->dfMinX = psTransform->padfGeoLocX[i];
                psTransform->dfYAtMinX = psTransform->padfGeoLocY[i];
            }
            if( psTransform->padfGeoLocX[i] > psTransform->dfMaxX )
            {
                psTransform->dfMaxX = psTransform->padfGeoLocX[i];
                psTransform->dfYAtMaxX = psTransform->padfGeoLocY[i];
            }
            if( psTransform->padfGeoLocY[i] < psTransform->dfMinY )
            {
                psTransform->dfMinY = psTransform->padfGeoLocY[i];
                psTransform->dfXAtMinY = psTransform->padfGeoLocX[i];
            }
            if( psTransform->padfGeoLocY[i] > psTransform->dfMaxY )
            {
                psTransform->dfMaxY = psTransform->padfGeoLocY[i];
                psTransform->dfXAtMaxY = psTransform->padfGeoLocX[i];
            }
        }
    }


    return true;
}

/************************************************************************/
/*                       GeoLocGenerateBackMap()                        */
/************************************************************************/

static bool GeoLocGenerateBackMap( GDALGeoLocTransformInfo *psTransform )

{
    const size_t nXSize = psTransform->nGeoLocXSize;
    const size_t nYSize = psTransform->nGeoLocYSize;

/* -------------------------------------------------------------------- */
/*      Decide on resolution for backmap.  We aim for slightly          */
/*      higher resolution than the source but we can't easily           */
/*      establish how much dead space there is in the backmap, so it    */
/*      is approximate.                                                 */
/* -------------------------------------------------------------------- */
    const double dfTargetPixels = (static_cast<double>(nXSize) * nYSize * OVERSAMPLE_FACTOR);
    const double dfPixelSize = sqrt(
        (psTransform->dfMaxX - psTransform->dfMinX) *
        (psTransform->dfMaxY - psTransform->dfMinY) / dfTargetPixels);
    if( dfPixelSize == 0.0 )
    {
        CPLError(CE_Failure, CPLE_AppDefined, "Invalid pixel size for backmap");
        return false;
    }

    const double dfBMXSize = (psTransform->dfMaxX - psTransform->dfMinX) / dfPixelSize + 1;
    const double dfBMYSize = (psTransform->dfMaxY - psTransform->dfMinY) / dfPixelSize + 1;

    if( !(dfBMXSize > 0 && dfBMXSize < INT_MAX) ||
        !(dfBMYSize > 0 && dfBMYSize < INT_MAX) )
    {
        CPLError(CE_Failure, CPLE_AppDefined, "Int overflow : %f x %f",
                 dfBMXSize, dfBMYSize);
        return false;
    }

    const size_t nBMXSize = static_cast<size_t>(dfBMXSize);
    const size_t nBMYSize = static_cast<size_t>(dfBMYSize);

    if( nBMYSize > std::numeric_limits<size_t>::max() / nBMXSize )
    {
        CPLError(CE_Failure, CPLE_AppDefined, "Int overflow : %f x %f",
                 dfBMXSize, dfBMYSize);
        return false;
    }

    psTransform->nBackMapWidth = nBMXSize;
    psTransform->nBackMapHeight = nBMYSize;

    const double dfMinX = psTransform->dfMinX - dfPixelSize / 2.0;
    const double dfMaxY = psTransform->dfMaxY + dfPixelSize / 2.0;

    psTransform->adfBackMapGeoTransform[0] = dfMinX;
    psTransform->adfBackMapGeoTransform[1] = dfPixelSize;
    psTransform->adfBackMapGeoTransform[2] = 0.0;
    psTransform->adfBackMapGeoTransform[3] = dfMaxY;
    psTransform->adfBackMapGeoTransform[4] = 0.0;
    psTransform->adfBackMapGeoTransform[5] = -dfPixelSize;

/* -------------------------------------------------------------------- */
/*      Allocate backmap, and initialize to nodata value (-1.0).        */
/* -------------------------------------------------------------------- */
    psTransform->pafBackMapX = static_cast<float *>(
        VSI_MALLOC3_VERBOSE(nBMXSize, nBMYSize, sizeof(float)));
    psTransform->pafBackMapY = static_cast<float *>(
        VSI_MALLOC3_VERBOSE(nBMXSize, nBMYSize, sizeof(float)));

    float *wgtsBackMap = static_cast<float *>(
        VSI_MALLOC3_VERBOSE(nBMXSize, nBMYSize, sizeof(float)));

    if( psTransform->pafBackMapX == nullptr ||
        psTransform->pafBackMapY == nullptr ||
        wgtsBackMap == nullptr)
    {
        CPLFree( wgtsBackMap );
        return false;
    }

    const size_t nBMXYCount = nBMXSize * nBMYSize;
    for( size_t i = 0; i < nBMXYCount; i++ )
    {
        psTransform->pafBackMapX[i] = 0;
        psTransform->pafBackMapY[i] = 0;
        wgtsBackMap[i] = 0.0;
    }

/* -------------------------------------------------------------------- */
/*      Run through the whole geoloc array forward projecting and       */
/*      pushing into the backmap.                                       */
/* -------------------------------------------------------------------- */

    const auto UpdateBackmap = [&](std::ptrdiff_t iBMX, std::ptrdiff_t iBMY,
                                   size_t iX, size_t iY,
                                   double tempwt)
    {
        const float fUpdatedBMX = psTransform->pafBackMapX[iBMX + iBMY * nBMXSize] +
            static_cast<float>( tempwt * (
                (iX + FSHIFT) * psTransform->dfPIXEL_STEP +
                psTransform->dfPIXEL_OFFSET));
        const float fUpdatedBMY = psTransform->pafBackMapY[iBMX + iBMY * nBMXSize] +
            static_cast<float>( tempwt * (
                (iY + FSHIFT) * psTransform->dfLINE_STEP +
                psTransform->dfLINE_OFFSET));
        const float fUpdatedWeight = wgtsBackMap[iBMX + iBMY * nBMXSize] +
                               static_cast<float>(tempwt);

        // Only update the backmap if the updated averaged value results in a
        // geoloc position that isn't too different from the original one.
        // (there's no guarantee that if padfGeoLocX[i] ~= padfGeoLoc[j],
        //  padfGeoLoc[alpha * i + (1 - alpha) * j] ~= padfGeoLoc[i] )
        if( fUpdatedWeight > 0 )
        {
            const float fX = fUpdatedBMX / fUpdatedWeight;
            const float fY = fUpdatedBMY / fUpdatedWeight;
            const double dfGeoLocPixel = (fX - psTransform->dfPIXEL_OFFSET) / psTransform->dfPIXEL_STEP;
            const double dfGeoLocLine = (fY - psTransform->dfLINE_OFFSET) / psTransform->dfLINE_STEP;
            size_t iXAvg = static_cast<size_t>(std::max(0.0, dfGeoLocPixel));
            iXAvg = std::min(iXAvg, psTransform->nGeoLocXSize-1);
            size_t iYAvg = static_cast<size_t>(std::max(0.0, dfGeoLocLine));
            iYAvg = std::min(iYAvg, psTransform->nGeoLocYSize-1);
            const double dfGLX = psTransform->padfGeoLocX[iXAvg + iYAvg * nXSize];
            const double dfGLY = psTransform->padfGeoLocY[iXAvg + iYAvg * nXSize];

            if( !(psTransform->bHasNoData &&
                  dfGLX == psTransform->dfNoDataX ) &&
                fabs(dfGLX - psTransform->padfGeoLocX[iX + iY * nXSize]) <= 2 * dfPixelSize &&
                fabs(dfGLY - psTransform->padfGeoLocY[iX + iY * nXSize]) <= 2 * dfPixelSize )
            {
                psTransform->pafBackMapX[iBMX + iBMY * nBMXSize] = fUpdatedBMX;
                psTransform->pafBackMapY[iBMX + iBMY * nBMXSize] = fUpdatedBMY;
                wgtsBackMap[iBMX + iBMY * nBMXSize] = fUpdatedWeight;
            }
        }
    };

    for( size_t iY = 0; iY < nYSize; iY++ )
    {
        for( size_t iX = 0; iX < nXSize; iX++ )
        {
            if( psTransform->bHasNoData &&
                psTransform->padfGeoLocX[iX + iY * nXSize]
                == psTransform->dfNoDataX )
                continue;

            const size_t i = iX + iY * nXSize;

            const double dBMX = static_cast<double>(
                    (psTransform->padfGeoLocX[i] - dfMinX) / dfPixelSize) - FSHIFT;

            const double dBMY = static_cast<double>(
                (dfMaxY - psTransform->padfGeoLocY[i]) / dfPixelSize) - FSHIFT;


            //Get top left index by truncation
            const std::ptrdiff_t iBMX = static_cast<std::ptrdiff_t>(dBMX);
            const std::ptrdiff_t iBMY = static_cast<std::ptrdiff_t>(dBMY);
            const double fracBMX = dBMX - iBMX;
            const double fracBMY = dBMY - iBMY;

            //Check if the center is in range
            if( iBMX < -1 || iBMY < -1 ||
                (iBMX > 0 && static_cast<size_t>(iBMX) > nBMXSize) ||
                (iBMY > 0 && static_cast<size_t>(iBMY) > nBMYSize) )
                continue;

            //Check logic for top left pixel
            if ((iBMX >= 0) && (iBMY >= 0) &&
                (static_cast<size_t>(iBMX) < nBMXSize) &&
                (static_cast<size_t>(iBMY) < nBMYSize))
            {
                const double tempwt = (1.0 - fracBMX) * (1.0 - fracBMY);
                UpdateBackmap(iBMX, iBMY, iX, iY, tempwt);
            }

            //Check logic for top right pixel
            if ((iBMY >= 0) &&
                (static_cast<size_t>(iBMX+1) < nBMXSize) &&
                (static_cast<size_t>(iBMY) < nBMYSize))
            {
                const double tempwt = fracBMX * (1.0 - fracBMY);
                UpdateBackmap(iBMX + 1, iBMY, iX, iY, tempwt);
            }

            //Check logic for bottom right pixel
            if ((static_cast<size_t>(iBMX+1) < nBMXSize) &&
                (static_cast<size_t>(iBMY+1) < nBMYSize))
            {
                const double tempwt = fracBMX * fracBMY;
                UpdateBackmap(iBMX + 1, iBMY + 1, iX, iY, tempwt);
            }

            //Check logic for bottom left pixel
            if ((iBMX >= 0) &&
                (static_cast<size_t>(iBMX) < nBMXSize) &&
                (static_cast<size_t>(iBMY+1) < nBMYSize))
            {
                const double tempwt = (1.0 - fracBMX) * fracBMY;
                UpdateBackmap(iBMX, iBMY + 1, iX, iY, tempwt);
            }

        }
    }


    //Each pixel in the backmap may have multiple entries.
    //We now go in average it out using the weights
    for( size_t i = 0; i < nBMXYCount; i++ )
    {
        //Check if pixel was only touch during neighbor scan
        //But no real weight was added as source point matched
        //backmap grid node
        if (wgtsBackMap[i] > 0)
        {
            psTransform->pafBackMapX[i] /= wgtsBackMap[i];
            psTransform->pafBackMapY[i] /= wgtsBackMap[i];
        }
        else
        {
            psTransform->pafBackMapX[i] = -1.0f;
            psTransform->pafBackMapY[i] = -1.0f;
        }
    }

    // Fill holes in backmap
    auto poMEMDS = std::unique_ptr<GDALDataset>(
          MEMDataset::Create( "",
                              static_cast<int>(nBMXSize),
                              static_cast<int>(nBMYSize),
                              0, GDT_Float32, nullptr ));

    for( int i = 1; i <= 2; i++ )
    {
        char szBuffer[32] = { '\0' };
        char szBuffer0[64] = { '\0' };
        char* apszOptions[] = { szBuffer0, nullptr };

        void* ptr = (i == 1) ? psTransform->pafBackMapX : psTransform->pafBackMapY;
        szBuffer[CPLPrintPointer(szBuffer, ptr, sizeof(szBuffer))] = '\0';
        snprintf(szBuffer0, sizeof(szBuffer0), "DATAPOINTER=%s", szBuffer);
        poMEMDS->AddBand(GDT_Float32, apszOptions);
        poMEMDS->GetRasterBand(i)->SetNoDataValue(-1);
    }

#ifdef DEBUG_GEOLOC
    if( CPLTestBool(CPLGetConfigOption("GEOLOC_DUMP", "NO")) )
    {
        GDALClose(GDALCreateCopy(GDALGetDriverByName("GTiff"),
                              "/tmp/geoloc_before_fill.tif",
                              poMEMDS.get(),
                              false, nullptr, nullptr, nullptr));
    }
#endif

    constexpr double dfMaxSearchDist = 3.0;
    constexpr int nSmoothingIterations = 1;
    for( int i = 1; i <= 2; i++ )
    {
        GDALFillNodata( GDALRasterBand::ToHandle(poMEMDS->GetRasterBand(i)),
                        nullptr,
                        dfMaxSearchDist,
                        0, // unused parameter
                        nSmoothingIterations,
                        nullptr,
                        nullptr,
                        nullptr );
    }

#ifdef DEBUG_GEOLOC
    if( CPLTestBool(CPLGetConfigOption("GEOLOC_DUMP", "NO")) )
    {
        GDALClose(GDALCreateCopy(GDALGetDriverByName("GTiff"),
                              "/tmp/geoloc_after_fill.tif",
                              poMEMDS.get(),
                              false, nullptr, nullptr, nullptr));
    }
#endif

    // A final hole filling logic, proceeding line by line, and feeling
    // holes when the backmap values surrounding the hole are close enough.
    for( size_t iBMY = 0; iBMY < nBMYSize; iBMY++ )
    {
        size_t iLastValidIX = static_cast<size_t>(-1);
        for( size_t iBMX = 0; iBMX < nBMXSize; iBMX++ )
        {
            const size_t iBM = iBMX + iBMY * nBMXSize;
            if( psTransform->pafBackMapX[iBM] < 0 )
                continue;
            if( iLastValidIX != static_cast<size_t>(-1) &&
                iBMX > iLastValidIX + 1 &&
                fabs( psTransform->pafBackMapX[iBM] -
                    psTransform->pafBackMapX[iLastValidIX + iBMY * nBMXSize]) <= 2 &&
                fabs( psTransform->pafBackMapY[iBM] -
                    psTransform->pafBackMapY[iLastValidIX + iBMY * nBMXSize]) <= 2 )
            {
                for( size_t iBMXInner = iLastValidIX + 1; iBMXInner < iBMX; ++iBMXInner )
                {
                    const float alpha = static_cast<float>(iBMXInner - iLastValidIX) / (iBMX - iLastValidIX);
                    psTransform->pafBackMapX[iBMXInner + iBMY * nBMXSize] =
                        (1.0f - alpha) * psTransform->pafBackMapX[iLastValidIX + iBMY * nBMXSize] +
                        alpha * psTransform->pafBackMapX[iBM];
                    psTransform->pafBackMapY[iBMXInner + iBMY * nBMXSize] =
                        (1.0f - alpha) * psTransform->pafBackMapY[iLastValidIX + iBMY * nBMXSize] +
                        alpha * psTransform->pafBackMapY[iBM];
                }
            }
            iLastValidIX = iBMX;
        }
    }

#ifdef DEBUG_GEOLOC
    if( CPLTestBool(CPLGetConfigOption("GEOLOC_DUMP", "NO")) )
    {
        GDALClose(GDALCreateCopy(GDALGetDriverByName("GTiff"),
                              "/tmp/geoloc_after_line_fill.tif",
                              poMEMDS.get(),
                              false, nullptr, nullptr, nullptr));
    }
#endif

    CPLFree( wgtsBackMap );

    return true;
}

/************************************************************************/
/*                       GDALGeoLocRescale()                            */
/************************************************************************/

static void GDALGeoLocRescale( char**& papszMD, const char* pszItem,
                               double dfRatio, double dfDefaultVal )
{
    const double dfVal =
        dfRatio *
        CPLAtofM(CSLFetchNameValueDef(papszMD, pszItem,
                                      CPLSPrintf("%.18g", dfDefaultVal)));

    papszMD = CSLSetNameValue(papszMD, pszItem, CPLSPrintf("%.18g", dfVal));

}

/************************************************************************/
/*                 GDALCreateSimilarGeoLocTransformer()                 */
/************************************************************************/

static
void* GDALCreateSimilarGeoLocTransformer( void *hTransformArg,
                                          double dfRatioX, double dfRatioY )
{
    VALIDATE_POINTER1(hTransformArg, "GDALCreateSimilarGeoLocTransformer",
                      nullptr);

    GDALGeoLocTransformInfo *psInfo =
        static_cast<GDALGeoLocTransformInfo *>(hTransformArg);

    char** papszGeolocationInfo = CSLDuplicate(psInfo->papszGeolocationInfo);

    if( dfRatioX != 1.0 || dfRatioY != 1.0 )
    {
        GDALGeoLocRescale(papszGeolocationInfo, "PIXEL_OFFSET", dfRatioX, 0.0);
        GDALGeoLocRescale(papszGeolocationInfo, "LINE_OFFSET", dfRatioY, 0.0);
        GDALGeoLocRescale(
            papszGeolocationInfo, "PIXEL_STEP", 1.0 / dfRatioX, 1.0);
        GDALGeoLocRescale(papszGeolocationInfo,
            "LINE_STEP", 1.0 / dfRatioY, 1.0);
    }

    psInfo = static_cast<GDALGeoLocTransformInfo*>(
        GDALCreateGeoLocTransformer(
            nullptr, papszGeolocationInfo, psInfo->bReversed));

    CSLDestroy(papszGeolocationInfo);

    return psInfo;
}

/************************************************************************/
/*                    GDALCreateGeoLocTransformer()                     */
/************************************************************************/

/** Create GeoLocation transformer */
void *GDALCreateGeoLocTransformer( GDALDatasetH hBaseDS,
                                   char **papszGeolocationInfo,
                                   int bReversed )

{

    if( CSLFetchNameValue(papszGeolocationInfo, "PIXEL_OFFSET") == nullptr
        || CSLFetchNameValue(papszGeolocationInfo, "LINE_OFFSET") == nullptr
        || CSLFetchNameValue(papszGeolocationInfo, "PIXEL_STEP") == nullptr
        || CSLFetchNameValue(papszGeolocationInfo, "LINE_STEP") == nullptr
        || CSLFetchNameValue(papszGeolocationInfo, "X_BAND") == nullptr
        || CSLFetchNameValue(papszGeolocationInfo, "Y_BAND") == nullptr )
    {
        CPLError( CE_Failure, CPLE_AppDefined,
                  "Missing some geolocation fields in "
                  "GDALCreateGeoLocTransformer()" );
        return nullptr;
    }

/* -------------------------------------------------------------------- */
/*      Initialize core info.                                           */
/* -------------------------------------------------------------------- */
    GDALGeoLocTransformInfo *psTransform =
        static_cast<GDALGeoLocTransformInfo *>(
            CPLCalloc(sizeof(GDALGeoLocTransformInfo), 1));

    psTransform->bReversed = CPL_TO_BOOL(bReversed);

    memcpy( psTransform->sTI.abySignature,
            GDAL_GTI2_SIGNATURE,
            strlen(GDAL_GTI2_SIGNATURE) );
    psTransform->sTI.pszClassName = "GDALGeoLocTransformer";
    psTransform->sTI.pfnTransform = GDALGeoLocTransform;
    psTransform->sTI.pfnCleanup = GDALDestroyGeoLocTransformer;
    psTransform->sTI.pfnSerialize = GDALSerializeGeoLocTransformer;
    psTransform->sTI.pfnCreateSimilar = GDALCreateSimilarGeoLocTransformer;

    psTransform->papszGeolocationInfo = CSLDuplicate( papszGeolocationInfo );

/* -------------------------------------------------------------------- */
/*      Pull geolocation info from the options/metadata.                */
/* -------------------------------------------------------------------- */
    psTransform->dfPIXEL_OFFSET =
        CPLAtof(CSLFetchNameValue( papszGeolocationInfo, "PIXEL_OFFSET" ));
    psTransform->dfLINE_OFFSET =
        CPLAtof(CSLFetchNameValue( papszGeolocationInfo, "LINE_OFFSET" ));
    psTransform->dfPIXEL_STEP =
        CPLAtof(CSLFetchNameValue( papszGeolocationInfo, "PIXEL_STEP" ));
    psTransform->dfLINE_STEP =
        CPLAtof(CSLFetchNameValue( papszGeolocationInfo, "LINE_STEP" ));

/* -------------------------------------------------------------------- */
/*      Establish access to geolocation dataset(s).                     */
/* -------------------------------------------------------------------- */
    const char *pszDSName = CSLFetchNameValue( papszGeolocationInfo,
                                               "X_DATASET" );
    if( pszDSName != nullptr )
    {
        CPLConfigOptionSetter oSetter("CPL_ALLOW_VSISTDIN", "NO", true);
        psTransform->hDS_X = GDALOpenShared( pszDSName, GA_ReadOnly );
    }
    else
    {
        psTransform->hDS_X = hBaseDS;
        if( hBaseDS )
        {
            GDALReferenceDataset( psTransform->hDS_X );
            psTransform->papszGeolocationInfo =
                CSLSetNameValue( psTransform->papszGeolocationInfo,
                                 "X_DATASET",
                                 GDALGetDescription( hBaseDS ) );
        }
    }

    pszDSName = CSLFetchNameValue( papszGeolocationInfo, "Y_DATASET" );
    if( pszDSName != nullptr )
    {
        CPLConfigOptionSetter oSetter("CPL_ALLOW_VSISTDIN", "NO", true);
        psTransform->hDS_Y = GDALOpenShared( pszDSName, GA_ReadOnly );
    }
    else
    {
        psTransform->hDS_Y = hBaseDS;
        if( hBaseDS )
        {
            GDALReferenceDataset( psTransform->hDS_Y );
            psTransform->papszGeolocationInfo =
                CSLSetNameValue( psTransform->papszGeolocationInfo,
                                 "Y_DATASET",
                                 GDALGetDescription( hBaseDS ) );
        }
    }

    if( psTransform->hDS_X == nullptr ||
        psTransform->hDS_Y == nullptr )
    {
        GDALDestroyGeoLocTransformer( psTransform );
        return nullptr;
    }

/* -------------------------------------------------------------------- */
/*      Get the band handles.                                           */
/* -------------------------------------------------------------------- */
    const int nXBand =
        std::max(1, atoi(CSLFetchNameValue( papszGeolocationInfo, "X_BAND" )));
    psTransform->hBand_X = GDALGetRasterBand( psTransform->hDS_X, nXBand );

    const int nYBand =
        std::max(1, atoi(CSLFetchNameValue( papszGeolocationInfo, "Y_BAND" )));
    psTransform->hBand_Y = GDALGetRasterBand( psTransform->hDS_Y, nYBand );

    if( psTransform->hBand_X == nullptr ||
        psTransform->hBand_Y == nullptr )
    {
        GDALDestroyGeoLocTransformer( psTransform );
        return nullptr;
    }

    psTransform->bSwapXY = CPLTestBool(CSLFetchNameValueDef(
        papszGeolocationInfo, "SWAP_XY", "NO"));

/* -------------------------------------------------------------------- */
/*     Check that X and Y bands have the same dimensions                */
/* -------------------------------------------------------------------- */
    const int nXSize_XBand = GDALGetRasterXSize( psTransform->hDS_X );
    const int nYSize_XBand = GDALGetRasterYSize( psTransform->hDS_X );
    const int nXSize_YBand = GDALGetRasterXSize( psTransform->hDS_Y );
    const int nYSize_YBand = GDALGetRasterYSize( psTransform->hDS_Y );
    if( nYSize_XBand == 1 || nYSize_YBand == 1 )
    {
        if( nYSize_XBand != 1 || nYSize_YBand != 1 )
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "X_BAND and Y_BAND should have both nYSize == 1");
            GDALDestroyGeoLocTransformer( psTransform );
            return nullptr;
        }
    }
    else if( nXSize_XBand != nXSize_YBand ||
             nYSize_XBand != nYSize_YBand )
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "X_BAND and Y_BAND do not have the same dimensions");
        GDALDestroyGeoLocTransformer( psTransform );
        return nullptr;
    }

    if( static_cast<size_t>(nXSize_XBand) >
            std::numeric_limits<size_t>::max() / nYSize_XBand )
    {
        CPLError(CE_Failure, CPLE_AppDefined, "Int overflow : %d x %d",
                 nXSize_XBand, nYSize_XBand);
        GDALDestroyGeoLocTransformer( psTransform );
        return nullptr;
    }

/* -------------------------------------------------------------------- */
/*      Load the geolocation array.                                     */
/* -------------------------------------------------------------------- */
    if( !GeoLocLoadFullData( psTransform )
        || !GeoLocGenerateBackMap( psTransform ) )
    {
        GDALDestroyGeoLocTransformer( psTransform );
        return nullptr;
    }

    return psTransform;
}

/************************************************************************/
/*                    GDALDestroyGeoLocTransformer()                    */
/************************************************************************/

/** Destroy GeoLocation transformer */
void GDALDestroyGeoLocTransformer( void *pTransformAlg )

{
    if( pTransformAlg == nullptr )
        return;

    GDALGeoLocTransformInfo *psTransform =
        static_cast<GDALGeoLocTransformInfo *>(pTransformAlg);

    CPLFree( psTransform->pafBackMapX );
    CPLFree( psTransform->pafBackMapY );
    CSLDestroy( psTransform->papszGeolocationInfo );
    CPLFree( psTransform->padfGeoLocX );
    CPLFree( psTransform->padfGeoLocY );

    if( psTransform->hDS_X != nullptr
        && GDALDereferenceDataset( psTransform->hDS_X ) == 0 )
            GDALClose( psTransform->hDS_X );

    if( psTransform->hDS_Y != nullptr
        && GDALDereferenceDataset( psTransform->hDS_Y ) == 0 )
            GDALClose( psTransform->hDS_Y );

    CPLFree( pTransformAlg );
}

/************************************************************************/
/*                        GDALGeoLocTransform()                         */
/************************************************************************/

/** Use GeoLocation transformer */
int GDALGeoLocTransform( void *pTransformArg,
                         int bDstToSrc,
                         int nPointCount,
                         double *padfX, double *padfY,
                         CPL_UNUSED double *padfZ,
                         int *panSuccess )
{
    GDALGeoLocTransformInfo *psTransform =
        static_cast<GDALGeoLocTransformInfo *>(pTransformArg);

    if( psTransform->bReversed )
        bDstToSrc = !bDstToSrc;

/* -------------------------------------------------------------------- */
/*      Do original pixel line to target geox/geoy.                     */
/* -------------------------------------------------------------------- */
    if( !bDstToSrc )
    {
        const size_t nXSize = psTransform->nGeoLocXSize;

        for( int i = 0; i < nPointCount; i++ )
        {
            if( padfX[i] == HUGE_VAL || padfY[i] == HUGE_VAL )
            {
                panSuccess[i] = FALSE;
                continue;
            }

            const double dfGeoLocPixel =
                (padfX[i] - psTransform->dfPIXEL_OFFSET)
                / psTransform->dfPIXEL_STEP;
            const double dfGeoLocLine =
                (padfY[i] - psTransform->dfLINE_OFFSET)
                / psTransform->dfLINE_STEP;

            size_t iX = static_cast<size_t>(std::max(0.0, dfGeoLocPixel));
            iX = std::min(iX, psTransform->nGeoLocXSize-1);
            size_t iY = static_cast<size_t>(std::max(0.0, dfGeoLocLine));
            iY = std::min(iY, psTransform->nGeoLocYSize-1);

            const double *padfGLX = psTransform->padfGeoLocX + iX + iY * nXSize;
            const double *padfGLY = psTransform->padfGeoLocY + iX + iY * nXSize;

            if( psTransform->bHasNoData &&
                padfGLX[0] == psTransform->dfNoDataX )
            {
                panSuccess[i] = FALSE;
                padfX[i] = HUGE_VAL;
                padfY[i] = HUGE_VAL;
                continue;
            }

            // This assumes infinite extension beyond borders of available
            // data based on closest grid square.

            if( iX + 1 < psTransform->nGeoLocXSize &&
                iY + 1 < psTransform->nGeoLocYSize &&
                (!psTransform->bHasNoData ||
                    (padfGLX[1] != psTransform->dfNoDataX &&
                     padfGLX[nXSize] != psTransform->dfNoDataX &&
                     padfGLX[nXSize + 1] != psTransform->dfNoDataX) ))
            {
                padfX[i] =
                    (1 - (dfGeoLocLine -iY))
                    * (padfGLX[0] +
                       (dfGeoLocPixel-iX) * (padfGLX[1] - padfGLX[0]))
                    + (dfGeoLocLine -iY)
                    * (padfGLX[nXSize] + (dfGeoLocPixel-iX) *
                       (padfGLX[nXSize+1] - padfGLX[nXSize]));
                padfY[i] =
                    (1 - (dfGeoLocLine -iY))
                    * (padfGLY[0] +
                       (dfGeoLocPixel-iX) * (padfGLY[1] - padfGLY[0]))
                    + (dfGeoLocLine -iY)
                    * (padfGLY[nXSize] + (dfGeoLocPixel-iX) *
                       (padfGLY[nXSize+1] - padfGLY[nXSize]));
            }
            else if( iX + 1 < psTransform->nGeoLocXSize &&
                     (!psTransform->bHasNoData ||
                        padfGLX[1] != psTransform->dfNoDataX) )
            {
                padfX[i] =
                    padfGLX[0] + (dfGeoLocPixel-iX) * (padfGLX[1] - padfGLX[0]);
                padfY[i] =
                    padfGLY[0] + (dfGeoLocPixel-iX) * (padfGLY[1] - padfGLY[0]);
            }
            else if( iY + 1 < psTransform->nGeoLocYSize &&
                     (!psTransform->bHasNoData ||
                        padfGLX[nXSize] != psTransform->dfNoDataX) )
            {
                padfX[i] = padfGLX[0]
                    + (dfGeoLocLine -iY) * (padfGLX[nXSize] - padfGLX[0]);
                padfY[i] = padfGLY[0]
                    + (dfGeoLocLine -iY) * (padfGLY[nXSize] - padfGLY[0]);
            }
            else
            {
                padfX[i] = padfGLX[0];
                padfY[i] = padfGLY[0];
            }

            if( psTransform->bSwapXY )
            {
                std::swap(padfX[i], padfY[i]);
            }

            panSuccess[i] = TRUE;
        }
    }

/* -------------------------------------------------------------------- */
/*      geox/geoy to pixel/line using backmap.                          */
/* -------------------------------------------------------------------- */
    else
    {
        for( int i = 0; i < nPointCount; i++ )
        {
            if( padfX[i] == HUGE_VAL || padfY[i] == HUGE_VAL )
            {
                panSuccess[i] = FALSE;
                continue;
            }

            if( psTransform->bSwapXY )
            {
                std::swap(padfX[i], padfY[i]);
            }

            const double dfBMX =
                ((padfX[i] - psTransform->adfBackMapGeoTransform[0])
                 / psTransform->adfBackMapGeoTransform[1]) - ISHIFT;
            const double dfBMY =
                ((padfY[i] - psTransform->adfBackMapGeoTransform[3])
                 / psTransform->adfBackMapGeoTransform[5]) - ISHIFT;

            // FIXME: in the case of ]-1,0[, dfBMX-iBMX will be wrong
            // We should likely error out if values are < 0 ==> affects a few
            // autotest results
            if( !(dfBMX > -1 && dfBMY > -1 &&
                  dfBMX < psTransform->nBackMapWidth &&
                  dfBMY < psTransform->nBackMapHeight) )
            {
                panSuccess[i] = FALSE;
                padfX[i] = HUGE_VAL;
                padfY[i] = HUGE_VAL;
                continue;
            }

            const std::ptrdiff_t iBMX = static_cast<std::ptrdiff_t>(dfBMX);
            const std::ptrdiff_t iBMY = static_cast<std::ptrdiff_t>(dfBMY);

            const size_t iBM = iBMX + iBMY * psTransform->nBackMapWidth;
            if( psTransform->pafBackMapX[iBM] < 0 )
            {
                panSuccess[i] = FALSE;
                padfX[i] = HUGE_VAL;
                padfY[i] = HUGE_VAL;
                continue;
            }

            const float* pafBMX = psTransform->pafBackMapX + iBM;
            const float* pafBMY = psTransform->pafBackMapY + iBM;

            if( static_cast<size_t>(iBMX + 1) < psTransform->nBackMapWidth &&
                static_cast<size_t>(iBMY + 1) < psTransform->nBackMapHeight &&
                pafBMX[1] >=0 && pafBMX[psTransform->nBackMapWidth] >= 0 &&
                pafBMX[psTransform->nBackMapWidth+1] >= 0)
            {
                padfX[i] =
                    (1-(dfBMY - iBMY))
                    * (pafBMX[0] + (dfBMX - iBMX) * (pafBMX[1] - pafBMX[0]))
                    + (dfBMY - iBMY)
                    * (pafBMX[psTransform->nBackMapWidth] +
                       (dfBMX - iBMX) * (pafBMX[psTransform->nBackMapWidth+1] -
                                         pafBMX[psTransform->nBackMapWidth]));
                padfY[i] =
                    (1-(dfBMY - iBMY))
                    * (pafBMY[0] + (dfBMX - iBMX) * (pafBMY[1] - pafBMY[0]))
                    + (dfBMY - iBMY)
                    * (pafBMY[psTransform->nBackMapWidth] +
                       (dfBMX - iBMX) * (pafBMY[psTransform->nBackMapWidth+1] -
                                         pafBMY[psTransform->nBackMapWidth]));
            }
            else if( static_cast<size_t>(iBMX + 1) < psTransform->nBackMapWidth &&
                     pafBMX[1] >=0)
            {
                padfX[i] = pafBMX[0] +
                            (dfBMX - iBMX) * (pafBMX[1] - pafBMX[0]);
                padfY[i] = pafBMY[0] +
                            (dfBMX - iBMX) * (pafBMY[1] - pafBMY[0]);
            }
            else if( static_cast<size_t>(iBMY + 1) < psTransform->nBackMapHeight &&
                     pafBMX[psTransform->nBackMapWidth] >= 0 )
            {
                padfX[i] =
                    pafBMX[0] +
                    (dfBMY - iBMY) * (pafBMX[psTransform->nBackMapWidth] -
                                      pafBMX[0]);
                padfY[i] =
                    pafBMY[0] +
                    (dfBMY - iBMY) * (pafBMY[psTransform->nBackMapWidth] -
                                      pafBMY[0]);
            }
            else
            {
                padfX[i] = pafBMX[0];
                padfY[i] = pafBMY[0];
            }
            panSuccess[i] = TRUE;
        }
    }

    return TRUE;
}

/************************************************************************/
/*                   GDALSerializeGeoLocTransformer()                   */
/************************************************************************/

CPLXMLNode *GDALSerializeGeoLocTransformer( void *pTransformArg )

{
    VALIDATE_POINTER1( pTransformArg, "GDALSerializeGeoLocTransformer", nullptr );

    GDALGeoLocTransformInfo *psInfo =
        static_cast<GDALGeoLocTransformInfo *>(pTransformArg);

    CPLXMLNode *psTree =
        CPLCreateXMLNode( nullptr, CXT_Element, "GeoLocTransformer" );

/* -------------------------------------------------------------------- */
/*      Serialize bReversed.                                            */
/* -------------------------------------------------------------------- */
    CPLCreateXMLElementAndValue(
        psTree, "Reversed",
        CPLString().Printf( "%d", static_cast<int>(psInfo->bReversed) ) );

/* -------------------------------------------------------------------- */
/*      geoloc metadata.                                                */
/* -------------------------------------------------------------------- */
    char **papszMD = psInfo->papszGeolocationInfo;
    CPLXMLNode *psMD= CPLCreateXMLNode( psTree, CXT_Element, "Metadata" );

    for( int i = 0; papszMD != nullptr && papszMD[i] != nullptr; i++ )
    {
        char *pszKey = nullptr;
        const char *pszRawValue = CPLParseNameValue( papszMD[i], &pszKey );

        CPLXMLNode *psMDI = CPLCreateXMLNode( psMD, CXT_Element, "MDI" );
        CPLSetXMLValue( psMDI, "#key", pszKey );
        CPLCreateXMLNode( psMDI, CXT_Text, pszRawValue );

        CPLFree( pszKey );
    }

    return psTree;
}

/************************************************************************/
/*                   GDALDeserializeGeoLocTransformer()                 */
/************************************************************************/

void *GDALDeserializeGeoLocTransformer( CPLXMLNode *psTree )

{
/* -------------------------------------------------------------------- */
/*      Collect metadata.                                               */
/* -------------------------------------------------------------------- */
    CPLXMLNode *psMetadata = CPLGetXMLNode( psTree, "Metadata" );

    if( psMetadata == nullptr ||
        psMetadata->eType != CXT_Element
        || !EQUAL(psMetadata->pszValue, "Metadata") )
        return nullptr;

    char **papszMD = nullptr;

    for( CPLXMLNode *psMDI = psMetadata->psChild;
         psMDI != nullptr;
         psMDI = psMDI->psNext )
    {
        if( !EQUAL(psMDI->pszValue, "MDI")
            || psMDI->eType != CXT_Element
            || psMDI->psChild == nullptr
            || psMDI->psChild->psNext == nullptr
            || psMDI->psChild->eType != CXT_Attribute
            || psMDI->psChild->psChild == nullptr )
            continue;

        papszMD =
            CSLSetNameValue( papszMD,
                             psMDI->psChild->psChild->pszValue,
                             psMDI->psChild->psNext->pszValue );
    }

/* -------------------------------------------------------------------- */
/*      Get other flags.                                                */
/* -------------------------------------------------------------------- */
    const int bReversed = atoi(CPLGetXMLValue(psTree, "Reversed", "0"));

/* -------------------------------------------------------------------- */
/*      Generate transformation.                                        */
/* -------------------------------------------------------------------- */
    void *pResult = GDALCreateGeoLocTransformer( nullptr, papszMD, bReversed );

/* -------------------------------------------------------------------- */
/*      Cleanup GCP copy.                                               */
/* -------------------------------------------------------------------- */
    CSLDestroy( papszMD );

    return pResult;
}
