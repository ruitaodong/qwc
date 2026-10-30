/*
 * GDAL raster driver for Philips iSyntax whole-slide images, backed by
 * libisyntax (https://github.com/amspath/libisyntax, BSD-2-Clause).
 *
 * Opens:
 *   /path/to/slide.isyntax               the slide
 *   ISYNTAX_ASSOC:label:/path/...         the label or macro image, listed
 *                                         as subdatasets
 *
 * The slide is exposed as 4 Byte bands (R, G, B, alpha) with its pyramid
 * levels as overviews. Unscanned areas are white and opaque, as libisyntax
 * returns them. Coordinates are level-0 pixels, origin top-left, y pointing
 * down the slide (geotransform 0,1,0,0,0,-1), the same convention as the
 * OpenSlide driver.
 *
 * Metadata: microns per pixel, scanner, acquisition date, compression, and
 * the slide's ICC profile (COLOR_PROFILE domain, SOURCE_ICC_PROFILE, base64,
 * as GDAL's TIFF/JPEG/PNG drivers report it). The barcode is withheld unless
 * ISYNTAX_EXPOSE_BARCODE=YES, since on clinical slides it often identifies
 * the specimen or patient.
 *
 * Configuration options:
 *   ISYNTAX_CACHE_SIZE      tiles kept decoded per open slide (default 500,
 *                           very roughly 0.4 MB each)
 *   ISYNTAX_EXPOSE_BARCODE  YES to report the barcode as metadata
 *
 * Like libisyntax itself, needs real files: no GDAL virtual file systems.
 */

#include "cpl_string.h"
#include "gdal_priv.h"

extern "C" {
#include "libisyntax.h"
}

#include <algorithm>
#include <climits>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#define IS_EXPORT extern "C" __attribute__((visibility("default")))

namespace
{

constexpr const char *ASSOC_PREFIX = "ISYNTAX_ASSOC:";

const GDALColorInterp COLOR_INTERP[] = {GCI_RedBand, GCI_GreenBand, GCI_BlueBand,
                                        GCI_AlphaBand};

bool InitLibrary()
{
    static std::once_flag once;
    static bool ok = false;
    std::call_once(once, [] { ok = libisyntax_init() == LIBISYNTAX_OK; });
    if (!ok)
        CPLError(CE_Failure, CPLE_AppDefined, "iSyntax: libisyntax_init() failed");
    return ok;
}

/* An open slide plus its tile cache; shared by the slide and its overviews. */
struct Slide
{
    isyntax_t *isyntax = nullptr;
    isyntax_cache_t *cache = nullptr;

    ~Slide()
    {
        if (cache)
            libisyntax_cache_destroy(cache);
        if (isyntax)
            libisyntax_close(isyntax);
    }
};
using SlidePtr = std::shared_ptr<Slide>;

isyntax_t *OpenFile(const char *path)
{
    if (!InitLibrary())
        return nullptr;
    isyntax_t *isyntax = nullptr;
    if (libisyntax_open(path, static_cast<libisyntax_open_flags_t>(0), &isyntax) != LIBISYNTAX_OK ||
        isyntax == nullptr)
    {
        CPLError(CE_Failure, CPLE_OpenFailed, "iSyntax: cannot open %s", path);
        return nullptr;
    }
    return isyntax;
}

SlidePtr OpenSlide(const char *path)
{
    auto slide = std::make_shared<Slide>();
    slide->isyntax = OpenFile(path);
    if (!slide->isyntax)
        return nullptr;
    const int cacheSize = std::max(16, atoi(CPLGetConfigOption("ISYNTAX_CACHE_SIZE", "500")));
    if (libisyntax_cache_create("gdal", cacheSize, &slide->cache) != LIBISYNTAX_OK ||
        libisyntax_cache_inject(slide->cache, slide->isyntax) != LIBISYNTAX_OK)
    {
        CPLError(CE_Failure, CPLE_OpenFailed, "iSyntax: cannot set up the tile cache for %s",
                 path);
        return nullptr;
    }
    return slide;
}

// libisyntax RGBA pixels are bytes R, G, B, A in memory.
inline GByte Channel(uint32_t pixel, int band)
{
    return reinterpret_cast<const GByte *>(&pixel)[band - 1];
}

/************************************************************************/
/*                         Slide (one pyramid level)                    */
/************************************************************************/

class ISyntaxDataset final : public GDALDataset
{
    friend class ISyntaxRasterBand;

    SlidePtr m_slide;
    int m_level = 0;
    std::vector<std::unique_ptr<ISyntaxDataset>> m_overviews;  // level 0 only

  public:
    ISyntaxDataset(SlidePtr slide, int level, int width, int height);

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

class ISyntaxRasterBand final : public GDALRasterBand
{
  public:
    ISyntaxRasterBand(ISyntaxDataset *ds, int band, int blockX, int blockY)
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
        return static_cast<int>(static_cast<ISyntaxDataset *>(poDS)->m_overviews.size());
    }

    GDALRasterBand *GetOverview(int i) override
    {
        auto &ovr = static_cast<ISyntaxDataset *>(poDS)->m_overviews;
        if (i < 0 || i >= static_cast<int>(ovr.size()))
            return nullptr;
        return ovr[i]->GetRasterBand(nBand);
    }

    CPLErr IReadBlock(int nBlockXOff, int nBlockYOff, void *pImage) override;
};

ISyntaxDataset::ISyntaxDataset(SlidePtr slide, int level, int width, int height)
    : m_slide(std::move(slide)), m_level(level)
{
    nRasterXSize = width;
    nRasterYSize = height;
    const int bx = std::clamp(libisyntax_get_tile_width(m_slide->isyntax), 64, 2048);
    const int by = std::clamp(libisyntax_get_tile_height(m_slide->isyntax), 64, 2048);
    for (int b = 1; b <= 4; ++b)
        SetBand(b, new ISyntaxRasterBand(this, b, bx, by));
    SetMetadataItem("INTERLEAVE", "PIXEL", "IMAGE_STRUCTURE");
}

CPLErr ISyntaxRasterBand::IReadBlock(int nBlockXOff, int nBlockYOff, void *pImage)
{
    auto *ds = static_cast<ISyntaxDataset *>(poDS);
    const int xOff = nBlockXOff * nBlockXSize;
    const int yOff = nBlockYOff * nBlockYSize;
    // Never read past the level: libisyntax doesn't bounds-check tile reads
    const int w = std::min(nBlockXSize, nRasterXSize - xOff);
    const int h = std::min(nBlockYSize, nRasterYSize - yOff);

    std::vector<uint32_t> rgba;
    try
    {
        rgba.resize(static_cast<size_t>(w) * h);
    }
    catch (const std::bad_alloc &)
    {
        CPLError(CE_Failure, CPLE_OutOfMemory, "iSyntax: out of memory");
        return CE_Failure;
    }

    CPLDebug("iSyntax", "level %d block (%d,%d) %dx%d", ds->m_level, nBlockXOff, nBlockYOff, w, h);
    // read_region takes coordinates in the level's own pixels
    if (libisyntax_read_region(ds->m_slide->isyntax, ds->m_slide->cache, ds->m_level, xOff, yOff,
                               w, h, rgba.data(), LIBISYNTAX_PIXEL_FORMAT_RGBA) != LIBISYNTAX_OK)
    {
        CPLError(CE_Failure, CPLE_FileIO, "iSyntax: reading level %d at %d,%d failed",
                 ds->m_level, xOff, yOff);
        return CE_Failure;
    }

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
            memset(dst, 0, blockBytes);  // partial edge block: transparent padding
        for (int y = 0; y < h; ++y)
        {
            const uint32_t *src = rgba.data() + static_cast<size_t>(y) * w;
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
/*                    Associated image (label, macro)                   */
/************************************************************************/

class ISyntaxAssocDataset final : public GDALDataset
{
    friend class ISyntaxAssocBand;
    std::vector<uint32_t> m_rgba;  // small images: decoded whole on open

  public:
    ISyntaxAssocDataset(std::vector<uint32_t> rgba, int w, int h);
};

class ISyntaxAssocBand final : public GDALRasterBand
{
  public:
    ISyntaxAssocBand(ISyntaxAssocDataset *ds, int band)
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
        const auto *ds = static_cast<ISyntaxAssocDataset *>(poDS);
        const uint32_t *src = ds->m_rgba.data() + static_cast<size_t>(nBlockYOff) * nBlockXSize;
        auto *dst = static_cast<GByte *>(pImage);
        for (int x = 0; x < nBlockXSize; ++x)
            dst[x] = Channel(src[x], nBand);
        return CE_None;
    }
};

ISyntaxAssocDataset::ISyntaxAssocDataset(std::vector<uint32_t> rgba, int w, int h)
    : m_rgba(std::move(rgba))
{
    nRasterXSize = w;
    nRasterYSize = h;
    for (int b = 1; b <= 4; ++b)
        SetBand(b, new ISyntaxAssocBand(this, b));
}

GDALDataset *OpenAssociated(const char *spec)
{
    // spec: <label|macro>:<path>
    const char *colon = strchr(spec, ':');
    if (colon == nullptr)
        return nullptr;
    const std::string name(spec, colon - spec);
    if (name != "label" && name != "macro")
    {
        CPLError(CE_Failure, CPLE_OpenFailed, "iSyntax: unknown associated image '%s'",
                 name.c_str());
        return nullptr;
    }
    std::unique_ptr<isyntax_t, void (*)(isyntax_t *)> isyntax(OpenFile(colon + 1), libisyntax_close);
    if (!isyntax)
        return nullptr;
    int32_t w = 0, h = 0;
    uint32_t *pixels = nullptr;
    const isyntax_error_t err =
        name == "label"
            ? libisyntax_read_label_image(isyntax.get(), &w, &h, &pixels, LIBISYNTAX_PIXEL_FORMAT_RGBA)
            : libisyntax_read_macro_image(isyntax.get(), &w, &h, &pixels, LIBISYNTAX_PIXEL_FORMAT_RGBA);
    if (err != LIBISYNTAX_OK || pixels == nullptr || w <= 0 || h <= 0 || w > 32768 || h > 32768)
    {
        free(pixels);
        CPLError(CE_Failure, CPLE_OpenFailed, "iSyntax: no usable %s image", name.c_str());
        return nullptr;
    }
    std::vector<uint32_t> rgba(pixels, pixels + static_cast<size_t>(w) * h);
    free(pixels);  // allocated by libisyntax's image decoder with malloc
    return new ISyntaxAssocDataset(std::move(rgba), w, h);
}

/************************************************************************/
/*                               Metadata                               */
/************************************************************************/

void SetIf(GDALDataset *ds, const char *key, const char *value)
{
    if (value != nullptr && *value != '\0')
        ds->SetMetadataItem(key, value);
}

void AddMetadata(ISyntaxDataset *ds, isyntax_t *isyntax, const char *path)
{
    if (libisyntax_get_is_mpp_known(isyntax))
    {
        ds->SetMetadataItem("isyntax.mpp-x", CPLSPrintf("%.17g", libisyntax_get_mpp_x(isyntax)));
        ds->SetMetadataItem("isyntax.mpp-y", CPLSPrintf("%.17g", libisyntax_get_mpp_y(isyntax)));
    }
    SetIf(ds, "isyntax.manufacturer", libisyntax_get_manufacturer(isyntax));
    SetIf(ds, "isyntax.model", libisyntax_get_manufacturers_model_name(isyntax));
    SetIf(ds, "isyntax.device-serial-number", libisyntax_get_device_serial_number(isyntax));
    SetIf(ds, "isyntax.acquisition-datetime", libisyntax_get_acquisition_datetime(isyntax));
    SetIf(ds, "isyntax.derivation-description", libisyntax_get_derivation_description(isyntax));
    for (int i = 0; i < libisyntax_get_software_versions_count(isyntax); ++i)
        SetIf(ds, CPLSPrintf("isyntax.software-version[%d]", i),
              libisyntax_get_software_versions(isyntax, i));
    ds->SetMetadataItem("isyntax.data-model-version",
                        CPLSPrintf("%d.%d", libisyntax_get_data_model_major_version(isyntax),
                                   libisyntax_get_data_model_minor_version(isyntax)));
    if (libisyntax_is_lossy_image_compression(isyntax))
    {
        ds->SetMetadataItem("isyntax.lossy-compression-ratio",
                            CPLSPrintf("%g", libisyntax_get_lossy_image_compression_ratio(isyntax)));
        SetIf(ds, "isyntax.lossy-compression-method",
              libisyntax_get_lossy_image_compression_method(isyntax));
    }
    if (CPLTestBool(CPLGetConfigOption("ISYNTAX_EXPOSE_BARCODE", "NO")))
        SetIf(ds, "isyntax.barcode", libisyntax_get_barcode(isyntax));

    // ICC profile, the way GDAL's other drivers report one
    uint8_t *icc = nullptr;
    uint32_t iccSize = 0;
    auto *wsi = const_cast<isyntax_image_t *>(libisyntax_get_wsi_image(isyntax));
    if (libisyntax_read_icc_profile(isyntax, wsi, &icc, &iccSize) == LIBISYNTAX_OK && icc != nullptr)
    {
        if (iccSize > 0)
        {
            char *b64 = CPLBase64Encode(static_cast<int>(iccSize), icc);
            ds->SetMetadataItem("SOURCE_ICC_PROFILE", b64, "COLOR_PROFILE");
            CPLFree(b64);
        }
        free(icc);  // allocated by libisyntax with malloc
    }

    CPLStringList subdatasets;
    int n = 0;
    for (const char *name : {"label", "macro"})
    {
        ++n;
        subdatasets.SetNameValue(CPLSPrintf("SUBDATASET_%d_NAME", n),
                                 CPLSPrintf("%s%s:%s", ASSOC_PREFIX, name, path));
        subdatasets.SetNameValue(CPLSPrintf("SUBDATASET_%d_DESC", n), name);
    }
    ds->SetMetadata(subdatasets.List(), "SUBDATASETS");
}

/************************************************************************/
/*                           Identify / Open                            */
/************************************************************************/

int ISyntaxDataset::Identify(GDALOpenInfo *poOpenInfo)
{
    if (STARTS_WITH_CI(poOpenInfo->pszFilename, ASSOC_PREFIX))
        return TRUE;
    // iSyntax files are an XML header followed by binary data
    return poOpenInfo->fpL != nullptr && EQUAL(CPLGetExtension(poOpenInfo->pszFilename), "isyntax") &&
           poOpenInfo->nHeaderBytes > 0 && poOpenInfo->pabyHeader[0] == '<';
}

GDALDataset *ISyntaxDataset::Open(GDALOpenInfo *poOpenInfo)
{
    if (!Identify(poOpenInfo))
        return nullptr;
    if (poOpenInfo->eAccess == GA_Update)
    {
        CPLError(CE_Failure, CPLE_NotSupported, "iSyntax: read-only driver");
        return nullptr;
    }
    const char *fn = poOpenInfo->pszFilename;
    if (STARTS_WITH_CI(fn, ASSOC_PREFIX))
        return OpenAssociated(fn + strlen(ASSOC_PREFIX));

    SlidePtr slide = OpenSlide(fn);
    if (!slide)
        return nullptr;

    const isyntax_image_t *wsi = libisyntax_get_wsi_image(slide->isyntax);
    const int levels = wsi ? libisyntax_image_get_level_count(wsi) : 0;
    if (levels < 1)
    {
        CPLError(CE_Failure, CPLE_OpenFailed, "iSyntax: no image levels in %s", fn);
        return nullptr;
    }
    const isyntax_level_t *base = libisyntax_image_get_level(wsi, 0);
    const int w = libisyntax_level_get_width(base), h = libisyntax_level_get_height(base);
    if (w <= 0 || h <= 0)
    {
        CPLError(CE_Failure, CPLE_OpenFailed, "iSyntax: unsupported dimensions in %s", fn);
        return nullptr;
    }

    auto ds = std::make_unique<ISyntaxDataset>(slide, 0, w, h);
    for (int level = 1; level < levels; ++level)
    {
        const isyntax_level_t *lvl = libisyntax_image_get_level(wsi, level);
        const int lw = libisyntax_level_get_width(lvl), lh = libisyntax_level_get_height(lvl);
        if (lw > 0 && lh > 0 && lw < w && lh < h)
            ds->m_overviews.push_back(std::make_unique<ISyntaxDataset>(slide, level, lw, lh));
    }
    AddMetadata(ds.get(), slide->isyntax, fn);
    ds->SetDescription(fn);
    return ds.release();
}

}  // namespace

/************************************************************************/
/*                             Registration                             */
/************************************************************************/

IS_EXPORT void GDALRegister_iSyntax()
{
    if (!GDAL_CHECK_VERSION("iSyntax driver"))
        return;
    if (GDALGetDriverByName("iSyntax") != nullptr)
        return;

    auto *driver = new GDALDriver();
    driver->SetDescription("iSyntax");
    driver->SetMetadataItem(GDAL_DCAP_RASTER, "YES");
    driver->SetMetadataItem(GDAL_DMD_LONGNAME, "Philips iSyntax whole-slide images (libisyntax)");
    driver->SetMetadataItem(GDAL_DMD_EXTENSION, "isyntax");
    driver->SetMetadataItem(GDAL_DMD_SUBDATASETS, "YES");
    driver->SetMetadataItem(GDAL_DMD_HELPTOPIC, "https://github.com/amspath/libisyntax");
    driver->pfnOpen = ISyntaxDataset::Open;
    driver->pfnIdentify = ISyntaxDataset::Identify;
    GetGDALDriverManager()->RegisterDriver(driver);
}

// Fallback entry point GDAL looks for in plugins
IS_EXPORT void GDALRegisterMe()
{
    GDALRegister_iSyntax();
}
