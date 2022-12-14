/*
 * @Description:
 * @version:
 * @Author: zwy
 * @Date: 2022-10-04 12:17:56
 * @LastEditors: zwy
 * @LastEditTime: 2022-11-11 16:02:45
 */

#include "yolo.h"

bool __check_cuda_runtime(cudaError_t code, const char *op, const char *file, int line)
{
    if (code != cudaSuccess)
    {
        const char *err_name = cudaGetErrorName(code);
        const char *err_message = cudaGetErrorString(code);
        printf("runtime error %s:%d  %s failed. \n  code = %s, message = %s\n", file, line, op, err_name, err_message);
        return false;
    }
    return true;
}

const char *severity_string(nvinfer1::ILogger::Severity t)
{
    switch (t)
    {
    case nvinfer1::ILogger::Severity::kINTERNAL_ERROR:
        return "[internal_error]";
    case nvinfer1::ILogger::Severity::kERROR:
        return "[error]";
    case nvinfer1::ILogger::Severity::kWARNING:
        return "[warning]";
    case nvinfer1::ILogger::Severity::kINFO:
        return "[info]";
    case nvinfer1::ILogger::Severity::kVERBOSE:
        return "[verbose]";
    default:
        return "[unkown]";
    }
}

static std::tuple<uint8_t, uint8_t, uint8_t> hsv2bgr(float h, float s, float v)
{
    const int h_i = static_cast<int>(h * 6);
    const float f = h * 6 - h_i;
    const float p = v * (1 - s);
    const float q = v * (1 - f * s);
    const float t = v * (1 - (1 - f) * s);
    float r, g, b;
    switch (h_i)
    {
    case 0:
        r = v;
        g = t;
        b = p;
        break;
    case 1:
        r = q;
        g = v;
        b = p;
        break;
    case 2:
        r = p;
        g = v;
        b = t;
        break;
    case 3:
        r = p;
        g = q;
        b = v;
        break;
    case 4:
        r = t;
        g = p;
        b = v;
        break;
    case 5:
        r = v;
        g = p;
        b = q;
        break;
    default:
        r = 1;
        g = 1;
        b = 1;
        break;
    }
    return std::make_tuple(static_cast<uint8_t>(b * 255), static_cast<uint8_t>(g * 255), static_cast<uint8_t>(r * 255));
}

static std::tuple<uint8_t, uint8_t, uint8_t> random_color(int id)
{
    float h_plane = ((((unsigned int)id << 2) ^ 0x937151) % 100) / 100.0f;
    ;
    float s_plane = ((((unsigned int)id << 3) ^ 0x315793) % 100) / 100.0f;
    return hsv2bgr(h_plane, s_plane, 1);
}

// ????????????????????????nv?????????????????????
// ?????????????????????????????????
template <typename _T>
std::shared_ptr<_T> make_nvshared(_T *ptr)
{
    return std::shared_ptr<_T>(ptr, [](_T *p)
                               { p->destroy(); });
}

bool exists(const std::string &path)
{
#ifdef _WIN32
    return ::PathFileExistsA(path.c_str());
#else
    return access(path.c_str(), R_OK) == 0;
#endif
}

bool build_model(const std::string &onnx_model_path, const std::string &trt_model_path, TRTLogger &logger)
{
    if (exists(trt_model_path))
    {
        printf("%s has exists.\n", trt_model_path.c_str());
        return true;
    }
    //??????????????????
    auto builder = make_nvshared(nvinfer1::createInferBuilder(logger));
    auto config = make_nvshared(builder->createBuilderConfig());
    auto network = make_nvshared(builder->createNetworkV2(1));

    // ??????onnxparser????????????????????????????????????network????????????addConv?????????????????????
    auto parser = make_nvshared(nvonnxparser::createParser(*network, logger));
    if (!parser->parseFromFile(onnx_model_path.c_str(), 1))
    {
        printf("Failed to parse %s\n", onnx_model_path.c_str());
        return false;
    }
    int maxBatchSize = 10;
    printf("Workspace Size = %.2f MB\n", (1 << 28) / 1024.0f / 1024.0f);
    config->setMaxWorkspaceSize(1 << 28);

    // ?????????????????????????????????????????????profile
    auto profile = builder->createOptimizationProfile();
    auto input_tensor = network->getInput(0);
    auto input_dims = input_tensor->getDimensions();

    // ????????????????????????????????????
    input_dims.d[0] = 1;
    profile->setDimensions(input_tensor->getName(), nvinfer1::OptProfileSelector::kMIN, input_dims);
    profile->setDimensions(input_tensor->getName(), nvinfer1::OptProfileSelector::kOPT, input_dims);
    input_dims.d[0] = maxBatchSize;
    profile->setDimensions(input_tensor->getName(), nvinfer1::OptProfileSelector::kMAX, input_dims);
    config->addOptimizationProfile(profile);

    //?????? engine
    auto engine = make_nvshared(builder->buildEngineWithConfig(*network, *config));
    if (engine == nullptr)
    {
        printf("Build engine failed.\n");
        return false;
    }

    // ???????????????????????????????????????
    auto model_data = make_nvshared(engine->serialize());
    FILE *f = fopen(trt_model_path.c_str(), "wb");
    fwrite(model_data->data(), 1, model_data->size(), f);
    fclose(f);

    // ????????????????????????????????????
    printf("Build Done.\n");
    return true;
}

std::vector<uint8_t> load_file(const std::string &file)
{
    std::ifstream in(file, std::ios::in | std::ios::binary);
    if (!in.is_open())
    {
        return {};
    }

    in.seekg(0, std::ios::end);
    size_t length = in.tellg();
    std::vector<uint8_t> data;
    if (length > 0)
    {
        in.seekg(0, std::ios::beg);
        data.resize(length);
        in.read((char *)&data[0], length);
    }
    in.close();
    return data;
}

std::vector<std::vector<float>> NMS(std::vector<std::vector<float>> &bboxes, float nms_threshold)
{
    std::sort(bboxes.begin(), bboxes.end(), [](std::vector<float> &a, std::vector<float> &b)
              { return a[5] > b[5]; });
    std::vector<bool> remove_flags(bboxes.size()); 
    std::vector<std::vector<float>> result_box;
    result_box.reserve(bboxes.size());

    auto iou = [](const std::vector<float> &a, const std::vector<float> &b)
    {
        float cross_left = std::max(a[0], b[0]);
        float cross_top = std::max(a[1], b[1]);
        float cross_right = std::min(a[2], b[2]);
        float cross_bottom = std::min(a[3], b[3]);

        float cross_area = std::max(0.0f, cross_right - cross_left) * std::max(0.0f, cross_bottom - cross_top);
        float union_area = std::max(0.0f, a[2] - a[0]) * std::max(0.0f, a[3] - a[1]) + std::max(0.0f, b[2] - b[0]) * std::max(0.0f, b[3] - b[1]) - cross_area;
        if (cross_area == 0 || union_area == 0)
            return 0.0f;
        return cross_area / union_area;
    };
    for (int i = 0; i < bboxes.size(); i++)
    {
        if (remove_flags[i])
            continue;
        auto &ibox = bboxes[i];
        result_box.emplace_back(ibox);
        for (int j = i + 1; j < bboxes.size(); j++)
        {
            if (remove_flags[i])
                continue;
            auto &jbox = bboxes[j];
            if (ibox[4] == jbox[4])
                if (iou(ibox, jbox) >= nms_threshold)
                    remove_flags[j] = true;
        }
    }
    return result_box;
}

bool inference(cv::Mat &src_image, const std::string &trt_model_path, const std::string &save_output_image_path, TRTLogger &logger, bool save_input_image)
{
    auto engine_data = load_file(trt_model_path);
    if (engine_data.size() == 0)
    {
        printf("loading TRT model is failed!\n");
        return false;
    }
    auto runtime = make_nvshared(nvinfer1::createInferRuntime(logger));
    auto engine = make_nvshared(runtime->deserializeCudaEngine(engine_data.data(), engine_data.size()));
    if (engine == nullptr)
    {
        printf("Deserialize cuda engine failed.\n");
        runtime->destroy();
        return false;
    }
    if (engine->getNbBindings() != 2)
    {
        printf("??????onnx???????????????, ?????????1????????????1?????????, ???????????????: %d?????????!\n", engine->getNbBindings() - 1);
        return false;
    }
    cudaStream_t stream = nullptr;
    checkRuntime(cudaStreamCreate(&stream));
    auto execution_context = make_nvshared(engine->createExecutionContext());

    int input_batch = 1;
    int input_channel = 3;
    int input_height = 640;
    int input_width = 640;
    int input_numel = input_batch * input_channel * input_height * input_width;
    float *input_data_host = nullptr;
    float *input_data_device = nullptr;

    checkRuntime(cudaMallocHost(&input_data_host, input_numel * sizeof(float)));
    checkRuntime(cudaMalloc(&input_data_device, input_numel * sizeof(float)));

    // letter box
    float scale_x = input_width / (float)src_image.cols;
    float scale_y = input_width / (float)src_image.rows;
    float scale = std::min(scale_x, scale_y);
    float i2d[6], d2i[6];
    // resize??????????????????????????????????????????????????????
    i2d[0] = scale;
    i2d[1] = 0;
    i2d[2] = (-scale * src_image.cols + input_width + scale - 1) * 0.5;
    i2d[3] = 0;
    i2d[4] = scale;
    i2d[5] = (-scale * src_image.rows + input_height + scale - 1) * 0.5;

    cv::Mat m2x3_i2d(2, 3, CV_32F, i2d);           // image to dst(network), 2x3 matrix
    cv::Mat m2x3_d2i(2, 3, CV_32F, d2i);           // dst to image, 2x3 matrix
    cv::invertAffineTransform(m2x3_i2d, m2x3_d2i); // ???????????????????????????

    cv::Mat input_image(input_height, input_width, CV_8UC3);
    cv::warpAffine(src_image, input_image, m2x3_i2d, input_image.size(), cv::INTER_LINEAR, cv::BORDER_CONSTANT, cv::Scalar::all(114)); // ????????????????????????????????????,??????
    if (save_input_image)
        cv::imwrite("../workspace/input-image.jpg", input_image);
    int image_area = input_image.cols * input_image.rows;

    // bgr -> rgb
    uint8_t *pimage = input_image.data;
    float *phost_b = input_data_host + image_area * 0;
    float *phost_g = input_data_host + image_area * 1;
    float *phost_r = input_data_host + image_area * 2;
    for (int i = 0; i < image_area; ++i, pimage += 3)
    {
        // ?????????????????????rgb?????????
        *phost_r++ = pimage[0] / 255.0f;
        *phost_g++ = pimage[1] / 255.0f;
        *phost_b++ = pimage[2] / 255.0f;
    }
    checkRuntime(cudaMemcpyAsync(input_data_device, input_data_host, input_numel * sizeof(float), cudaMemcpyHostToDevice, stream));

    auto output_dims = engine->getBindingDimensions(1);
    int output_numbox = output_dims.d[1];
    int output_numprob = output_dims.d[2];
    int num_classes = output_numprob - 5;
    int output_numel = input_batch * output_numbox * output_numprob;
    float *output_data_host = nullptr;
    float *output_data_device = nullptr;
    checkRuntime(cudaMallocHost(&output_data_host, sizeof(float) * output_numel));
    checkRuntime(cudaMalloc(&output_data_device, sizeof(float) * output_numel));

    // ???????????????????????????????????????????????????
    auto input_dims = engine->getBindingDimensions(0);
    input_dims.d[0] = input_batch;

    execution_context->setBindingDimensions(0, input_dims);
    float *bindings[] = {input_data_device, output_data_device};
    bool success = execution_context->enqueueV2((void **)bindings, stream, nullptr);
    checkRuntime(cudaMemcpyAsync(output_data_host, output_data_device, sizeof(float) * output_numel, cudaMemcpyDeviceToHost, stream));
    checkRuntime(cudaStreamSynchronize(stream));

    // decode box?????????????????????????????????????????????????????????(??????:???????????????????????????????????????
    std::vector<std::vector<float>> bboxes;
    float confidence_threshold = 0.25;
    float nms_threshold = 0.5;
    for (int i = 0; i < output_numbox; i++)
    {
        float *ptr = output_data_host + i * output_numprob;
        float objness = ptr[4];
        if (objness < confidence_threshold)
            continue;

        float *pclass = ptr + 5;
        int label = std::max_element(pclass, pclass + num_classes) - pclass;
        float prob = pclass[label];
        float confidence = prob * objness;
        if (confidence < confidence_threshold)
            continue;

        // ?????????????????????
        float cx = ptr[0];
        float cy = ptr[1];
        float width = ptr[2];
        float height = ptr[3];

        // ?????????
        float left = cx - width * 0.5;
        float top = cy - height * 0.5;
        float right = cx + width * 0.5;
        float bottom = cy + height * 0.5;

        // ?????????????????????
        float image_base_left = d2i[0] * left + d2i[2];
        float image_base_right = d2i[0] * right + d2i[2];
        float image_base_top = d2i[0] * top + d2i[5];
        float image_base_bottom = d2i[0] * bottom + d2i[5];
        bboxes.push_back({image_base_left, image_base_top, image_base_right, image_base_bottom, (float)label, confidence});
    }
    printf("decoded bboxes.size = %ld\n", bboxes.size());

    // NMS???????????????
    std::vector<std::vector<float>> box_result = NMS(bboxes, confidence_threshold);
    printf("box_result.size = %ld\n", box_result.size());

    for (int i = 0; i < box_result.size(); i++)
    {
        auto &ibox = box_result[i];
        float left = ibox[0];
        float top = ibox[1];
        float right = ibox[2];
        float bottom = ibox[3];

        int class_label = ibox[4];
        float confidence = ibox[5];

        cv::Scalar color;
        std::tie(color[0], color[1], color[2]) = random_color(class_label);
        cv::rectangle(src_image, cv::Point(left, top), cv::Point(right, bottom), color, 10);

        auto name = cocolabels[class_label];
        auto caption = cv::format("%s  %.2f", name, confidence);
        int text_width = cv::getTextSize(caption, 0, 1, 2, nullptr).width + 30;
        cv::rectangle(src_image, cv::Point(left - 3, top - 33), cv::Point(left + text_width, top), color, -1);
        cv::putText(src_image, caption, cv::Point(left, top - 5), 0, 1, cv::Scalar::all(0), 10, 16);
    }
    cv::imwrite(save_output_image_path, src_image);
    printf("save output image: %s\n", save_output_image_path.c_str());
    checkRuntime(cudaStreamDestroy(stream));
    checkRuntime(cudaFreeHost(input_data_host));
    checkRuntime(cudaFreeHost(output_data_host));
    checkRuntime(cudaFree(input_data_device));
    checkRuntime(cudaFree(output_data_device));
}
