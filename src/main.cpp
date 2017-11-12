#include <chrono>
#include <cxxopts.hpp>
#include <exif.h>
#include <hevcimagefilereader.hpp>
#include <log.hpp>
#include <vips/vips8>

extern "C" {
    #include <libavutil/opt.h>
    #include <libavutil/imgutils.h>
    #include <libavcodec/avcodec.h>
    #include <libswscale/swscale.h>
};

using namespace std;
using namespace vips;

typedef ImageFileReaderInterface::DataVector DataVector;
typedef ImageFileReaderInterface::IdVector IdVector;
typedef ImageFileReaderInterface::GridItem GridItem;
typedef ImageFileReaderInterface::FileReaderException FileReaderException;


// Global vars
static bool VERBOSE = false;
static int QUALITY = 90;

static struct SwsContext* swsContext;


/**
 * Check if image has a grid configuration and return the grid id
 * @param reader
 * @param contextId
 * @return
 */
IdVector findGridItems(const HevcImageFileReader *reader, uint32_t contextId)
{
    IdVector gridItemIds;
    reader->getItemListByType(contextId, "grid", gridItemIds);

    if (gridItemIds.empty()) {
        throw logic_error("No grid items founds!");
    }

    return gridItemIds;
}

/**
 * Find thmb reference in metabox
 * @param reader
 * @param contextId
 * @param itemId
 * @return
 */
uint32_t findThumbnailId(const HevcImageFileReader *reader, uint32_t contextId, uint32_t itemId)
{
    IdVector thmbIds;
    reader->getReferencedToItemListByType(contextId, itemId, "thmb", thmbIds);

    if (thmbIds.empty()) {
        throw logic_error("Thumbnail ID not found!");
    }

    return thmbIds.at(0);
}

/**
 * Convert colorspace of decoded frame load into buffer
 * @param frame
 * @param dst
 * @param dst_size
 * @return the number of bytes written to dst, or a negative value on error
 */
int copyFrameInto(AVFrame *frame, uint8_t *dst, size_t dst_size)
{
    AVFrame* imgFrame = av_frame_alloc();
    int width = frame->width;
    int height = frame->height;

    uint8_t *tempBuffer = (uint8_t*) av_malloc(dst_size);

    struct SwsContext *sws_ctx = sws_getCachedContext(swsContext,
                                                      width, height, AV_PIX_FMT_YUV420P,
                                                      width, height, AV_PIX_FMT_RGB24,
                                                      0, nullptr, nullptr, nullptr);

    av_image_fill_arrays(imgFrame->data, imgFrame->linesize, tempBuffer, AV_PIX_FMT_RGB24, width, height, 1);
    uint8_t const* const* frameDataPtr = (uint8_t const* const*)frame->data;

    // Convert YUV to RGB
    sws_scale(sws_ctx, frameDataPtr, frame->linesize, 0, height, imgFrame->data, imgFrame->linesize);

    // Move RGB data in pixel order into memory
    const uint8_t* const* dataPtr = static_cast<const uint8_t* const*>(imgFrame->data);
    int size = static_cast<int>(dst_size);
    int ret = av_image_copy_to_buffer(dst, size, dataPtr, imgFrame->linesize, AV_PIX_FMT_RGB24, width, height, 1);

    av_free(imgFrame);
    av_free(tempBuffer);

    return ret;
}

/**
 * Get libav HEVC decoder
 * @return
 */
AVCodecContext* getHEVCDecoderContext()
{
    AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_HEVC);
    AVCodecContext *c = avcodec_alloc_context3(codec);

    if (!c) {
        throw logic_error("Could not allocate video codec context");
    }

    if (avcodec_open2(c, c->codec, nullptr) < 0) {
        throw logic_error("Could not open codec");
    }

    return c;
}

/**
 * Decode HEVC Frame using libav
 * @param hevcData
 * @param frame
 * @return negative value on error, otherwise the bytes used
 */
int decodeHEVCFrame(AVCodecContext* c, DataVector& hevcData, AVFrame* frame)
{
    AVPacket avpkt = {};
    av_init_packet(&avpkt);
    avpkt.size = static_cast<int>(hevcData.size());
    avpkt.data = &hevcData[0];

    int success;
    int ret = avcodec_decode_video2(c, frame, &success, &avpkt);
    if (ret < 0) {
        throw logic_error("Failed to decode frame, return value: " + to_string(ret));
    }

    return success;
}


/**
 * Extract EXIF data from HEIF
 * @param reader
 * @param contextId
 * @param itemId
 * @return
 */
easyexif::EXIFInfo extractExifData(HevcImageFileReader *reader, uint32_t contextId, uint32_t itemId)
{
    IdVector exifItemIds;
    DataVector exifData;

    reader->getReferencedToItemListByType(contextId, itemId, "cdsc", exifItemIds);

    if (exifItemIds.empty()) {
        throw logic_error("Exif Data ID (cdsc) not found!");
    }

    reader->getItemData(contextId, exifItemIds.at(0), exifData);

    if (exifData.empty()) {
        throw logic_error("Exif data is empty");
    }

    easyexif::EXIFInfo exifInfo;

    const int exifOffset = 4; // TODO: Derive this from data!
    uint8_t* exifDataPtr = &exifData[exifOffset];
    uint32_t exifDataLength = static_cast<uint32_t>(exifData.size() - exifOffset);

    int parseRet = exifInfo.parseFromEXIFSegment(exifDataPtr, exifDataLength);

    if (parseRet != 0) {
        throw logic_error("Failed to parse EXIF data!");
    }

    return exifInfo;
}


/**
 *
 * @param inputFilename
 * @param outputFilename
 * @return
 */
int exportThumbnail(const string& inputFilename, const string& outputFilename)
{
    HevcImageFileReader reader;
    reader.initialize(inputFilename);
    const uint32_t contextId = reader.getFileProperties().rootLevelMetaBoxProperties.contextId;

    // Detect grid
    const IdVector& gridItems = findGridItems(&reader, contextId);

    uint32_t gridItemId = gridItems.at(0);

    // Find Thumbnail ID
    const uint32_t thmbId = findThumbnailId(&reader, contextId, gridItemId);

    // Extract EXIF data;
    easyexif::EXIFInfo exifInfo = extractExifData(&reader, contextId, gridItemId);

    // Get thumbnail HEVC data
    DataVector hevcData;
    reader.getItemDataWithDecoderParameters(contextId, thmbId, hevcData);

    // Decode HEVC Frame
    AVCodecContext* decoder = getHEVCDecoderContext();
    AVFrame* frame = av_frame_alloc();

    if (!decodeHEVCFrame(decoder, hevcData, frame)) {
        throw logic_error("Failed to decode HEVC thumbnail");
    }

    size_t bufferSize = static_cast<size_t>(avpicture_get_size(AV_PIX_FMT_RGB24, frame->width, frame->height));
    uint8_t* rgbBuffer = (uint8_t*)malloc(bufferSize);
    copyFrameInto(frame, rgbBuffer, bufferSize);

    // Load image into vips and save as JPEG
    VImage thumbImg = VImage::new_from_memory(rgbBuffer, bufferSize, frame->width, frame->height, 3, VIPS_FORMAT_UCHAR);

    thumbImg.set(VIPS_META_ORIENTATION, exifInfo.Orientation);

    char * jpegName = const_cast<char *>(outputFilename.c_str());
    thumbImg.jpegsave(jpegName, VImage::option()->set("Q", QUALITY));

    free(rgbBuffer);
    avcodec_close(decoder);
    av_free(decoder);
    av_free(frame);

    return 0;
}


/**
 *
 * @param inputFilename
 * @param outputFilename
 * @return
 */
int convertToJpeg(const string& inputFilename, const string& outputFilename)
{
    HevcImageFileReader reader;
    reader.initialize(inputFilename);
    const uint32_t contextId = reader.getFileProperties().rootLevelMetaBoxProperties.contextId;

    // Detect grid
    const IdVector& gridItems = findGridItems(&reader, contextId);

    uint32_t gridItemId = gridItems.at(0);
    GridItem gridItem;
    gridItem = reader.getItemGrid(contextId, gridItemId);

    // Convenience vars
    uint32_t width = gridItem.outputWidth;
    uint32_t height = gridItem.outputHeight;
    uint32_t columns = gridItem.columnsMinusOne + 1;
    uint32_t rows = gridItem.rowsMinusOne + 1;

    if (VERBOSE) {
        cout << "Grid is " << width << "x" << height << " pixels in tiles " << columns << "x" << rows << endl;
    }

    // Extract EXIF data;
    easyexif::EXIFInfo exifInfo = extractExifData(&reader, contextId, gridItemId);

    // Find master tiles to extract
    IdVector tileItemIds;
    reader.getItemListByType(contextId, "master", tileItemIds);

    uint32_t firstTileId = tileItemIds.at(0);

    // Extract and decode all tiles

    AVCodecContext* decoder = getHEVCDecoderContext();
    AVFrame* frame = av_frame_alloc();

    chrono::steady_clock::time_point begin_encode = chrono::steady_clock::now();

    vector<VImage> tiles;

    for (uint32_t tileItemId : tileItemIds) {

        DataVector hevcData;
        reader.getItemDataWithDecoderParameters(contextId, tileItemId, firstTileId, hevcData);

        if (!decodeHEVCFrame(decoder, hevcData, frame)) {
            throw logic_error("Failed to decode HEVC tile #" + to_string(tileItemId));
        }

        size_t bufferSize = static_cast<size_t>(avpicture_get_size(AV_PIX_FMT_RGB24, frame->width, frame->height));
        uint8_t* rgbBuffer = (uint8_t*)malloc(bufferSize);
        copyFrameInto(frame, rgbBuffer, bufferSize);

        // Load image into vips and save as JPEG
        VImage img = VImage::new_from_memory(rgbBuffer, bufferSize, frame->width, frame->height, 3, VIPS_FORMAT_UCHAR);

        tiles.push_back(img);
    }

    avcodec_close(decoder);
    av_free(decoder);
    av_free(frame);

    chrono::steady_clock::time_point end_encode = chrono::steady_clock::now();
    long tileEncodeTime = chrono::duration_cast<chrono::milliseconds>(end_encode - begin_encode).count();

    if (VERBOSE) {
        cout << "Export & encode tiles " << tileEncodeTime << "ms" << endl;
    }

    // Stitch tiles together

    chrono::steady_clock::time_point begin_buildImage = chrono::steady_clock::now();

    VImage result = VImage::new_memory();

    result = result.arrayjoin(tiles, VImage::option()->set("across", (int)columns));

    result = result.extract_area(0, 0, width, height);

    result.set(VIPS_META_ORIENTATION, exifInfo.Orientation);

    char * jpegName = const_cast<char *>(outputFilename.c_str());
    result.jpegsave(jpegName, VImage::option()->set("Q", QUALITY));

    chrono::steady_clock::time_point end_buildImage = chrono::steady_clock::now();
    long buildImageTime = chrono::duration_cast<chrono::milliseconds>(end_buildImage - begin_buildImage).count();

    if (VERBOSE) {
        cout << "Building image " << buildImageTime << "ms" << endl;
    }

    return 0;
}

int main(int argc, char* argv[])
{
    // Disable colr and pixi boxes unknown warnings from libheif
    Log::getWarningInstance().setLevel(Log::LogLevel::ERROR);

    int retval = -1;

    VIPS_INIT(argv[0]);

    avcodec_register_all();

    try {

        cxxopts::Options options(argv[0], "Convert HEIF images to JPEG");

        options.positional_help("input_file output_file");

        options.parse_positional(vector<string>{"input", "output"});

        options.add_options()
                ("i,input", "Input HEIF image", cxxopts::value<string>())
                ("o,output", "Output JPEG image", cxxopts::value<string>())
                ("q,quality", "Output JPEG quality (1-100) default 90", cxxopts::value<int>(QUALITY))
                ("v,verbose", "Verbose output", cxxopts::value<bool>(VERBOSE))
                ("t,thumbnail", "Export thumbnail", cxxopts::value<bool>())
                ;

        options.parse(argc, argv);

        if (options.count("input") && options.count("output")) {
            string inputFileName = options["input"].as<string>();
            string outputFileName = options["output"].as<string>();
            bool thumb = options["thumbnail"].as<bool>();

            chrono::steady_clock::time_point begin = chrono::steady_clock::now();

            if (thumb) {
                retval = exportThumbnail(inputFileName, outputFileName);
            } else {
                retval = convertToJpeg(inputFileName, outputFileName);
            }

            chrono::steady_clock::time_point end = chrono::steady_clock::now();
            long duration = chrono::duration_cast<chrono::milliseconds>(end - begin).count();

            if (VERBOSE) {
                cout << "Total Time " << duration << "ms" << endl;
            }
        } else {
            cout << options.help() << endl;
        }


    }
    catch (const cxxopts::OptionException& oe) {
        cout << "error parsing options: " << oe.what() << endl;
    }
    catch (const FileReaderException& fre) {
        cerr << "Could not read HEIF image: " << fre.what() << endl;
    }
    catch (const logic_error& le) {
        cerr << le.what() << endl;
    }

    vips_shutdown();

    return retval;
}

