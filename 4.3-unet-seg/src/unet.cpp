/*
 * @Description:
 * @version:
 * @Author: zwy
 * @Date: 2022-10-05 09:49:51
 * @LastEditors: zwy
 * @LastEditTime: 2022-10-05 15:09:14
 */
#include "unet.h"

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

// 通过智能指针管理nv返回的指针参数
// 内存自动释放，避免泄漏
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

// 上一节的代码
bool build_model(const std::string &trt_model_file, const std::string &onnx_model_file, TRTLogger &logger)
{

    if (exists(trt_model_file))
    {
        printf("\033[33m[warning]:%s has exists.\033[0m\n", trt_model_file.c_str());
        return true;
    }

    // 这是基本需要的组件
    auto builder = make_nvshared(nvinfer1::createInferBuilder(logger));
    auto config = make_nvshared(builder->createBuilderConfig());

    // createNetworkV2(1)表示采用显性batch size，新版tensorRT(>=7.0)时，不建议采用0非显性batch size
    // 因此贯穿以后，请都采用createNetworkV2(1)而非createNetworkV2(0)或者createNetwork
    auto network = make_nvshared(builder->createNetworkV2(1));

    // 通过onnxparser解析器解析的结果会填充到network中，类似addConv的方式添加进去
    auto parser = make_nvshared(nvonnxparser::createParser(*network, logger));
    if (!parser->parseFromFile(onnx_model_file.c_str(), 1))
    {
        printf("\033[31m[error]:Failed to parse %s \033[0m\n", onnx_model_file.c_str());

        // 注意这里的几个指针还没有释放，是有内存泄漏的，后面考虑更优雅的解决
        return false;
    }

    int maxBatchSize = 10;
    printf("[info]:Workspace Size = %.2f MB\n", (1 << 28) / 1024.0f / 1024.0f);
    config->setMaxWorkspaceSize(1 << 28);

    // 如果模型有多个输入，则必须多个profile
    auto profile = builder->createOptimizationProfile();
    auto input_tensor = network->getInput(0);
    auto input_dims = input_tensor->getDimensions();

    // 配置最小允许batch
    input_dims.d[0] = 1;
    profile->setDimensions(input_tensor->getName(), nvinfer1::OptProfileSelector::kMIN, input_dims);
    profile->setDimensions(input_tensor->getName(), nvinfer1::OptProfileSelector::kOPT, input_dims);

    // 配置最大允许batch
    // if networkDims.d[i] != -1, then minDims.d[i] == optDims.d[i] == maxDims.d[i] == networkDims.d[i]
    input_dims.d[0] = maxBatchSize;
    profile->setDimensions(input_tensor->getName(), nvinfer1::OptProfileSelector::kMAX, input_dims);
    config->addOptimizationProfile(profile);

    auto engine = make_nvshared(builder->buildEngineWithConfig(*network, *config));
    if (engine == nullptr)
    {
        printf("\033[31m[error]:Build engine failed. \033[0m\n");
        return false;
    }

    // 将模型序列化，并储存为文件
    auto model_data = make_nvshared(engine->serialize());
    FILE *f = fopen(trt_model_file.c_str(), "wb");
    fwrite(model_data->data(), 1, model_data->size(), f);
    fclose(f);

    // 卸载顺序按照构建顺序倒序
    printf("[info]:Build Done.\n");
    return true;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////
std::vector<uint8_t> load_file(const std::string &file)
{
    std::ifstream in(file, std::ios::in | std::ios::binary);
    if (!in.is_open())
        return {};

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
void RGB2BGR(cv::Mat &src_RGB, cv::Mat &out_BGR)
{
    cv::cvtColor(src_RGB, out_BGR, cv::COLOR_BGR2RGB);
}
static std::tuple<cv::Mat, cv::Mat> post_process(float *output, int output_width, int output_height, int num_class, int ibatch)
{

    cv::Mat output_prob(output_height, output_width, CV_32F);
    cv::Mat output_index(output_height, output_width, CV_8U);

    float *pnet = output + ibatch * output_width * output_height * num_class;
    float *prob = output_prob.ptr<float>(0);
    uint8_t *pidx = output_index.ptr<uint8_t>(0);

    for (int k = 0; k < output_prob.cols * output_prob.rows; ++k, pnet += num_class, ++prob, ++pidx)
    {
        int ic = std::max_element(pnet, pnet + num_class) - pnet;
        *prob = pnet[ic];
        *pidx = ic;
    }
    return std::make_tuple(output_prob, output_index);
}

static void render(cv::Mat &image, const cv::Mat &prob, const cv::Mat &iclass)
{

    auto pimage = image.ptr<cv::Vec3b>(0);
    auto pprob = prob.ptr<float>(0);
    auto pclass = iclass.ptr<uint8_t>(0);

    for (int i = 0; i < image.cols * image.rows; ++i, ++pimage, ++pprob, ++pclass)
    {

        int iclass = *pclass;
        float probability = *pprob;
        auto &pixel = *pimage;
        float foreground = std::min(0.6f + probability * 0.2f, 0.8f);
        float background = 1 - foreground;
        for (int c = 0; c < 3; ++c)
        {
            auto value = pixel[c] * background + foreground * _classes_colors[iclass * 3 + 2 - c];
            pixel[c] = std::min((int)value, 255);
        }
    }
}

void inference(cv::Mat &image, const std::string &trt_model_file, const std::string &save_result_image, TRTLogger &logger)
{
    auto engine_data = load_file(trt_model_file);
    auto runtime = make_nvshared(nvinfer1::createInferRuntime(logger));
    auto engine = make_nvshared(runtime->deserializeCudaEngine(engine_data.data(), engine_data.size()));
    if (engine == nullptr)
    {
        printf("\033[31m[error]:Deserialize cuda engine failed.\033[0m\n");
        runtime->destroy();
        return;
    }
    if (engine->getNbBindings() != 2)
    {
        printf("\033[31m[error]:你的onnx导出有问题,必须是1个输入和1个输出,你这明显有:%d个输出.\033[0m\n", engine->getNbBindings() - 1);
        return;
    }
    cudaStream_t stream = nullptr;
    checkRuntime(cudaStreamCreate(&stream));
    auto execution_context = make_nvshared(engine->createExecutionContext());

    int input_batch = 1;
    int input_channel = 3;
    int input_height = 512;
    int input_width = 512;
    int input_numel = input_batch * input_channel * input_height * input_width;
    float *input_data_host = nullptr;
    float *input_data_device = nullptr;
    checkRuntime(cudaMallocHost(&input_data_host, input_numel * sizeof(float)));
    checkRuntime(cudaMalloc(&input_data_device, input_numel * sizeof(float)));
    ///////////////////////////////////////////////////
    float scale_x = input_width / (float)image.cols;
    float scale_y = input_height / (float)image.rows;
    float scale = std::min(scale_x, scale_y);
    float i2d[6], d2i[6];
    i2d[0] = scale;
    i2d[1] = 0;
    i2d[2] = (-scale * image.cols + input_width + scale - 1) * 0.5;
    i2d[3] = 0;
    i2d[4] = scale;
    i2d[5] = (-scale * image.rows + input_height + scale - 1) * 0.5;

    cv::Mat m2x3_i2d(2, 3, CV_32F, i2d);
    cv::Mat m2x3_d2i(2, 3, CV_32F, d2i);
    cv::invertAffineTransform(m2x3_i2d, m2x3_d2i);

    cv::Mat input_image(input_height, input_width, CV_8UC3);
    cv::Mat input_image_host(input_height, input_width, CV_8UC3);
    cv::warpAffine(image, input_image, m2x3_i2d, input_image.size(), cv::INTER_LINEAR, cv::BORDER_CONSTANT, cv::Scalar::all(114));
    cv::imwrite("../workspace/input-image.jpg", input_image);
    int image_area = input_image.cols * input_image.rows;
    unsigned char *pimage = input_image.data;
    float *phost_b = input_data_host + image_area * 0;
    float *phost_g = input_data_host + image_area * 1;
    float *phost_r = input_data_host + image_area * 2;
    for (int i = 0; i < image_area; ++i, pimage += 3)
    {
        *phost_r++ = pimage[0] / 255.0f;
        *phost_g++ = pimage[1] / 255.0f;
        *phost_b++ = pimage[2] / 255.0f;
    }
    ///////////////////////////////////////////////////
    checkRuntime(cudaMemcpyAsync(input_data_device, input_data_host, input_numel * sizeof(float), cudaMemcpyHostToDevice, stream));

    // 3x3输入，对应3x3输出
    auto output_dims = engine->getBindingDimensions(1);
    int output_height = output_dims.d[1];
    int output_width = output_dims.d[2];
    int num_classes = output_dims.d[3];
    int output_numel = input_batch * output_height * output_width * num_classes;
    float *output_data_host = nullptr;
    float *output_data_device = nullptr;
    checkRuntime(cudaMallocHost(&output_data_host, sizeof(float) * output_numel));
    checkRuntime(cudaMalloc(&output_data_device, sizeof(float) * output_numel));

    // 明确当前推理时，使用的数据输入大小
    auto input_dims = engine->getBindingDimensions(0);
    input_dims.d[0] = input_batch;

    execution_context->setBindingDimensions(0, input_dims);
    float *bindings[] = {input_data_device, output_data_device};
    bool success = execution_context->enqueueV2((void **)bindings, stream, nullptr);
    checkRuntime(cudaMemcpyAsync(output_data_host, output_data_device, sizeof(float) * output_numel, cudaMemcpyDeviceToHost, stream));
    checkRuntime(cudaStreamSynchronize(stream));

    cv::Mat prob, iclass;
    std::tie(prob, iclass) = post_process(output_data_host, output_width, output_height, num_classes, 0);
    cv::warpAffine(prob, prob, m2x3_d2i, image.size(), cv::INTER_LINEAR);
    cv::warpAffine(iclass, iclass, m2x3_d2i, image.size(), cv::INTER_NEAREST);
    render(image, prob, iclass);

    printf("[info]:Done, Save to %s\n", save_result_image.c_str());
    cv::imwrite(save_result_image, image);

    checkRuntime(cudaStreamDestroy(stream));
    checkRuntime(cudaFreeHost(input_data_host));
    checkRuntime(cudaFreeHost(output_data_host));
    checkRuntime(cudaFree(input_data_device));
    checkRuntime(cudaFree(output_data_device));
}
