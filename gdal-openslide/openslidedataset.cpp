/*
 * GDAL raster driver for whole-slide images, backed by OpenSlide.
 *
 * Opens:
 *   OPENSLIDE:/path/to/slide.svs      the slide (any format OpenSlide reads,
 *                                     including generic tiled TIFF)
 *   /path/to/slide.mrxs               same, without prefix, for files whose
 *                                     vendor OpenSlide recognises (not
 *                                     generic TIFF, so GeoTIFFs are left alone)
 *   OPENSLIDE_ASSOC:label:/path/...   an associated image (label, macro,
 *                                     thumbnail), listed as subdatasets
 *
 * The slide is exposed as 4 Byte bands (R, G, B, alpha) with OpenSlide's
 * pyramid levels as overviews. Coordinates are level-0 pixels with the
 * origin top-left and y pointing down the slide (geotransform 0,1,0,0,0,-1),
 * so the image appears upright in GIS software. All OpenSlide properties
 * (openslide.mpp-x, aperio.AppMag, ...) are reported as metadata.
 *
 * Targets the OpenSlide 3.4 API (what Debian ships) and GDAL >= 3.0.
 */

#include "cpl_string.h"
#include "gdal_priv.h"

#include <openslide.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#define OS_EXPORT extern "C" __attribute__((visibility("default")))

namespace
{

constexpr const char *PREFIX = "OPENSLIDE:";
constexpr const char *ASSOC_PREFIX = "OPENSLIDE_ASSOC:";
constexpr const char *EXTENSIONS = "svs tif tiff vms vmu ndpi scn mrxs svslide bif";

using SlidePtr = std::shared_ptr<openslide_t>;

SlidePtr OpenSlide(const char *path)
{
    openslide_t *osr = openslide_open(path);
    if (osr == nullptr)
        return nullptr;  // not a format OpenSlide knows
    SlidePtr slide(osr, openslide_close);
    if (const char *err = openslide_get_error(osr))
    {
        CPLError(CE_Failure, CPLE_OpenFailed, "OpenSlide: %s: %s", path, err);
        return nullptr;
    }
    return slide;
}

bool CheckSlide(openslide_t *osr)
{
    if (const char *err = openslide_get_error(osr))
    {
        CPLError(CE_Failure, CPLE_FileIO, "OpenSlide: %s", err);
        return false;
    }
    return true;
}

// OpenSlide pixels are premultiplied ARGB in native byte order.
inline GByte Channel(uint32_t argb, int band)
{
    const unsigned a = argb >> 24;
    if (band == 4)
        return static_cast<GByte>(a);
    const unsigned c = (argb >> (8 * (3 - band))) & 0xff;  // band 1=R 2=G 3=B
    if (a == 255 || a == 0)
        return static_cast<GByte>(a == 0 ? 0 : c);
    return static_cast<GByte>(std::min(255u, (c * 255 + a / 2) / a));
}

const GDALColorInterp COLOR_INTERP[] = {GCI_RedBand, GCI_GreenBand, GCI_BlueBand,
                                        GCI_AlphaBand};

int TileSize(openslide_t *osr, int level, const char *axis)
{
    const char *value = openslide_get_property_value(
        osr, CPLSPrintf("openslide.level[%d].tile-%s", level, axis));
    const int size = value ? atoi(value) : 0;
    return size >= 64 && size <= 2048 ? size : 256;
}

/************************************************************************/
/*                         Slide (one pyramid level)                    */
/************************************************************************/

class OpenSlideDataset final : public GDALDataset
{
    friend class OpenSlideRasterBand;

    SlidePtr m_slide;
    int m_level = 0;
    double m_downsample = 1.0;
    std::vector<std::unique_ptr<OpenSlideDataset>> m_overviews;  // level 0 only

  public:
    OpenSlideDataset(SlidePtr slide, int level, int64_t width, int64_t height);

    static int Identify(GDALOpenInfo *poOpenInfo);
    static GDALDataset *Open(GDALOpenInfo *poOpenInfo);

    CPLErr GetGeoTransform(double *gt) override
    {
        if (m_level != 0)
            return CE_Failure;
        gt[0] = 0.0; gt[1] = 1.0; gt[2] = 0.0;
        gt[3] = 0.0; gt[4] = 0.0; gt[5] = -1.0;
        return CE_None;
    }
};

class OpenSlideRasterBand final : public GDALRasterBand
{
  public:
    OpenSlideRasterBand(OpenSlideDataset *ds, int band, int blockX, int blockY)
    {
        poDS = ds;
        nBand = band;
        eDataType = GDT_Byte;
        nBlockXSize = blockX;
        nBlockYSize = blockY;
    }

    GDALColorInterp GetColorInterpretation() override { return COLOR_INTERP[nBand - 1]; }

    int GetOverviewCount() override
    {
        return static_cast<int>(static_cast<OpenSlideDataset *>(poDS)->m_overviews.size());
    }

    GDALRasterBand *GetOverview(int i) override
    {
        auto &ovr = static_cast<OpenSlideDataset *>(poDS)->m_overviews;
        if (i < 0 || i >= static_cast<int>(ovr.size()))
            return nullptr;
        return ovr[i]->GetRasterBand(nBand);
    }

    CPLErr IReadBlock(int nBlockXOff, int nBlockYOff, void *pImage) override;
};

OpenSlideDataset::OpenSlideDataset(SlidePtr slide, int level, int64_t width, int64_t height)
    : m_slide(std::move(slide)), m_level(level)
{
    nRasterXSize = static_cast<int>(width);
    nRasterYSize = static_cast<int>(height);
    m_downsample = openslide_get_level_downsample(m_slide.get(), level);
    const int bx = TileSize(m_slide.get(), level, "width");
    const int by = TileSize(m_slide.get(), level, "height");
    for (int b = 1; b <= 4; ++b)
        SetBand(b, new OpenSlideRasterBand(this, b, bx, by));
    SetMetadataItem("INTERLEAVE", "PIXEL", "IMAGE_STRUCTURE");
}

CPLErr OpenSlideRasterBand::IReadBlock(int nBlockXOff, int nBlockYOff, void *pImage)
{
    auto *ds = static_cast<OpenSlideDataset *>(poDS);
    const int xOff = nBlockXOff * nBlockXSize;
    const int yOff = nBlockYOff * nBlockYSize;
    const int w = std::min(nBlockXSize, nRasterXSize - xOff);
    const int h = std::min(nBlockYSize, nRasterYSize - yOff);

    std::vector<uint32_t> argb;
    try
    {
        argb.resize(static_cast<size_t>(w) * h);
    }
    catch (const std::bad_alloc &)
    {
        CPLError(CE_Failure, CPLE_OutOfMemory, "OpenSlide: out of memory");
        return CE_Failure;
    }

    CPLDebug("OpenSlide", "level %d block (%d,%d) %dx%d", ds->m_level, nBlockXOff, nBlockYOff,
             w, h);
    // read_region takes the top-left corner in level-0 coordinates
    openslide_read_region(ds->m_slide.get(), argb.data(),
                          std::llround(xOff * ds->m_downsample),
                          std::llround(yOff * ds->m_downsample), ds->m_level, w, h);
    if (!CheckSlide(ds->m_slide.get()))
        return CE_Failure;

    // One read serves all four bands: fill the other bands' blocks too.
    const size_t blockBytes = static_cast<size_t>(nBlockXSize) * nBlockYSize;
    for (int b = 1; b <= 4; ++b)
    {
        GByte *dst = nullptr;
        GDALRasterBlock *block = nullptr;
        if (b == nBand)
            dst = static_cast<GByte *>(pImage);
        else
        {
            GDALRasterBand *other = ds->GetRasterBand(b);
            block = other->TryGetLockedBlockRef(nBlockXOff, nBlockYOff);
            if (block != nullptr)
            {  // already cached
                block->DropLock();
                continue;
            }
            block = other->GetLockedBlockRef(nBlockXOff, nBlockYOff, TRUE);
            if (block == nullptr)
                continue;
            dst = static_cast<GByte *>(block->GetDataRef());
        }
        if (w < nBlockXSize || h < nBlockYSize)
            memset(dst, 0, blockBytes);  // partial edge block
        for (int y = 0; y < h; ++y)
        {
            const uint32_t *src = argb.data() + static_cast<size_t>(y) * w;
            GByte *row = dst + static_cast<size_t>(y) * nBlockXSize;
            for (int x = 0; x < w; ++x)
                row[x] = Channel(src[x], b);
        }
        if (block != nullptr)
            block->DropLock();
    }
    return CE_None;
}

/************************************************************************/
/*              Associated image (label, macro, thumbnail)              */
/************************************************************************/

class OpenSlideAssocDataset final : public GDALDataset
{
    friend class OpenSlideAssocBand;
    std::vector<uint32_t> m_argb;  // small images: read whole on open

  public:
    OpenSlideAssocDataset(std::vector<uint32_t> argb, int w, int h);
};

class OpenSlideAssocBand final : public GDALRasterBand
{
  public:
    OpenSlideAssocBand(OpenSlideAssocDataset *ds, int band)
    {
        poDS = ds;
        nBand = band;
        eDataType = GDT_Byte;
        nBlockXSize = ds->GetRasterXSize();
        nBlockYSize = 1;
    }
    GDALColorInterp GetColorInterpretation() override { return COLOR_INTERP[nBand - 1]; }
    CPLErr IReadBlock(int, int nBlockYOff, void *pImage) override
    {
        const auto *ds = static_cast<OpenSlideAssocDataset *>(poDS);
        const uint32_t *src = ds->m_argb.data() + static_cast<size_t>(nBlockYOff) * nBlockXSize;
        auto *dst = static_cast<GByte *>(pImage);
        for (int x = 0; x < nBlockXSize; ++x)
            dst[x] = Channel(src[x], nBand);
        return CE_None;
    }
};

OpenSlideAssocDataset::OpenSlideAssocDataset(std::vector<uint32_t> argb, int w, int h)
    : m_argb(std::move(argb))
{
    nRasterXSize = w;
    nRasterYSize = h;
    for (int b = 1; b <= 4; ++b)
        SetBand(b, new OpenSlideAssocBand(this, b));
}

GDALDataset *OpenAssociated(const char *spec)
{
    // spec: <name>:<path>  (OpenSlide's associated image names have no ':')
    const char *colon = strchr(spec, ':');
    if (colon == nullptr)
        return nullptr;
    const std::string name(spec, colon - spec);
    SlidePtr slide = OpenSlide(colon + 1);
    if (!slide)
        return nullptr;
    int64_t w = -1, h = -1;
    openslide_get_associated_image_dimensions(slide.get(), name.c_str(), &w, &h);
    if (w <= 0 || h <= 0 || w > 32768 || h > 32768)
    {
        CPLError(CE_Failure, CPLE_OpenFailed, "OpenSlide: no usable associated image '%s'",
                 name.c_str());
        return nullptr;
    }
    std::vector<uint32_t> argb(static_cast<size_t>(w) * h);
    openslide_read_associated_image(slide.get(), name.c_str(), argb.data());
    if (!CheckSlide(slide.get()))
        return nullptr;
    return new OpenSlideAssocDataset(std::move(argb), static_cast<int>(w), static_cast<int>(h));
}

/************************************************************************/
/*                           Identify / Open                            */
/************************************************************************/

int OpenSlideDataset::Identify(GDALOpenInfo *poOpenInfo)
{
    const char *fn = poOpenInfo->pszFilename;
    if (STARTS_WITH_CI(fn, PREFIX) || STARTS_WITH_CI(fn, ASSOC_PREFIX))
        return TRUE;
    if (poOpenInfo->fpL == nullptr)
        return FALSE;  // OpenSlide needs a real file
    const CPLStringList exts(CSLTokenizeString(EXTENSIONS));
    if (exts.FindString(CPLGetExtension(fn)) < 0)
        return FALSE;
    // Without the prefix, claim only real slide formats: generic tiled TIFF
    // would also match ordinary (Geo)TIFFs.
    const char *vendor = openslide_detect_vendor(fn);
    return vendor != nullptr && !EQUAL(vendor, "generic-tiff");
}

GDALDataset *OpenSlideDataset::Open(GDALOpenInfo *poOpenInfo)
{
    if (!Identify(poOpenInfo))
        return nullptr;
    if (poOpenInfo->eAccess == GA_Update)
    {
        CPLError(CE_Failure, CPLE_NotSupported, "OpenSlide: read-only driver");
        return nullptr;
    }
    const char *fn = poOpenInfo->pszFilename;
    if (STARTS_WITH_CI(fn, ASSOC_PREFIX))
        return OpenAssociated(fn + strlen(ASSOC_PREFIX));
    const char *path = STARTS_WITH_CI(fn, PREFIX) ? fn + strlen(PREFIX) : fn;

    SlidePtr slide = OpenSlide(path);
    if (!slide)
        return nullptr;

    const int levels = openslide_get_level_count(slide.get());
    int64_t w = 0, h = 0;
    openslide_get_level0_dimensions(slide.get(), &w, &h);
    if (levels < 1 || w <= 0 || h <= 0 || w > INT_MAX || h > INT_MAX)
    {
        CPLError(CE_Failure, CPLE_OpenFailed, "OpenSlide: unsupported dimensions in %s", path);
        return nullptr;
    }

    auto ds = std::make_unique<OpenSlideDataset>(slide, 0, w, h);
    for (int level = 1; level < levels; ++level)
    {
        int64_t lw = 0, lh = 0;
        openslide_get_level_dimensions(slide.get(), level, &lw, &lh);
        if (lw > 0 && lh > 0 && lw < w && lh < h)
            ds->m_overviews.push_back(std::make_unique<OpenSlideDataset>(slide, level, lw, lh));
    }

    for (const char *const *p = openslide_get_property_names(slide.get()); p && *p; ++p)
        ds->SetMetadataItem(*p, openslide_get_property_value(slide.get(), *p));

    CPLStringList subdatasets;
    int n = 0;
    for (const char *const *p = openslide_get_associated_image_names(slide.get()); p && *p; ++p)
    {
        int64_t aw = 0, ah = 0;
        openslide_get_associated_image_dimensions(slide.get(), *p, &aw, &ah);
        ++n;
        subdatasets.SetNameValue(CPLSPrintf("SUBDATASET_%d_NAME", n),
                                 CPLSPrintf("%s%s:%s", ASSOC_PREFIX, *p, path));
        subdatasets.SetNameValue(CPLSPrintf("SUBDATASET_%d_DESC", n),
                                 CPLSPrintf("%s (%lldx%lld)", *p, static_cast<long long>(aw),
                                            static_cast<long long>(ah)));
    }
    if (n > 0)
        ds->SetMetadata(subdatasets.List(), "SUBDATASETS");

    ds->SetDescription(fn);
    return ds.release();
}

}  // namespace

/************************************************************************/
/*                             Registration                             */
/************************************************************************/

OS_EXPORT void GDALRegister_OpenSlide()
{
    if (!GDAL_CHECK_VERSION("OpenSlide driver"))
        return;
    if (GDALGetDriverByName("OpenSlide") != nullptr)
        return;

    auto *driver = new GDALDriver();
    driver->SetDescription("OpenSlide");
    driver->SetMetadataItem(GDAL_DCAP_RASTER, "YES");
    driver->SetMetadataItem(GDAL_DMD_LONGNAME, "Whole-slide images (OpenSlide)");
    driver->SetMetadataItem(GDAL_DMD_EXTENSIONS, EXTENSIONS);
    driver->SetMetadataItem(GDAL_DMD_CONNECTION_PREFIX, PREFIX);
    driver->SetMetadataItem(GDAL_DMD_SUBDATASETS, "YES");
    driver->SetMetadataItem(GDAL_DMD_HELPTOPIC, "https://openslide.org/");
    driver->SetMetadataItem("OPENSLIDE_VERSION", openslide_get_version());
    driver->pfnOpen = OpenSlideDataset::Open;
    driver->pfnIdentify = OpenSlideDataset::Identify;
    GetGDALDriverManager()->RegisterDriver(driver);
}

// Fallback entry point GDAL looks for in plugins
OS_EXPORT void GDALRegisterMe()
{
    GDALRegister_OpenSlide();
}
