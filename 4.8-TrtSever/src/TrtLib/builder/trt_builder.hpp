

#ifndef TRT_BUILDER_HPP
#define TRT_BUILDER_HPP

#include <string>
#include <vector>
#include <functional>
#include "../infer/trt_infer.hpp"

namespace TRT
{

    typedef std::function<void(int current, int count, const std::vector<std::string> &files, std::shared_ptr<Tensor> &tensor)> Int8Process;
    typedef std::function<std::vector<int64_t>(const std::string &name, const std::vector<int64_t> &shape)> LayerHookFuncReshape;

    enum class ModelSourceType : int
    {
        OnnX,
        OnnXData
    };

    class ModelSource
    {
    public:
        ModelSource() = default;
        ModelSource(const std::string &onnxmodel);
        ModelSource(const char *onnxmodel);
        ModelSourceType type() const;
        std::string onnxmodel() const;
        std::string descript() const;
        const void *onnx_data() const;
        size_t onnx_data_size() const;

        static ModelSource onnx(const std::string &file)
        {
            ModelSource output;
            output.onnxmodel_ = file;
            output.type_ = ModelSourceType::OnnX;
            return output;
        }

        static ModelSource onnx_data(const void *ptr, size_t size)
        {
            ModelSource output;
            output.onnx_data_ = ptr;
            output.onnx_data_size_ = size;
            output.type_ = ModelSourceType::OnnXData;
            return output;
        }

    private:
        std::string onnxmodel_;
        const void *onnx_data_ = nullptr;
        size_t onnx_data_size_ = 0;
        ModelSourceType type_;
    };

    enum class CompileOutputType : int
    {
        File,
        Memory
    };

    class CompileOutput
    {
    public:
        CompileOutput(CompileOutputType type = CompileOutputType::Memory);
        CompileOutput(const std::string &file);
        CompileOutput(const char *file);
        void set_data(const std::vector<uint8_t> &data);
        void set_data(std::vector<uint8_t> &&data);

        const std::vector<uint8_t> &data() const { return data_; };
        CompileOutputType type() const { return type_; }
        std::string file() const { return file_; }

    private:
        CompileOutputType type_ = CompileOutputType::Memory;
        std::vector<uint8_t> data_;
        std::string file_;
    };

    class InputDims
    {
    public:
        InputDims() = default;

        // ??????-1??????????????????????????????????????????
        InputDims(const std::initializer_list<int> &dims);
        InputDims(const std::vector<int> &dims);

        const std::vector<int> &dims() const;

    private:
        std::vector<int> dims_;
    };

    enum class Mode : int
    {
        FP32,
        FP16,
        INT8
    };

    const char *mode_string(Mode type);

    void set_layer_hook_reshape(const LayerHookFuncReshape &func);

    /** ?????????INT8????????????int8process????????????
         int8ImageDirectory???int8EntropyCalibratorFile??????????????????
         ??????????????????????????????int8EntropyCalibratorFile???calibrator????????????int8EntropyCalibratorFile???????????????
         ?????????????????????????????????int8EntropyCalibratorFile???calibrator??????int8EntropyCalibratorFile?????????????????????????????????
              ???int8ImageDirectory???????????????????????????
        ?????????FP32??????FP16??????int8process???int8ImageDirectory???int8EntropyCalibratorFile??????????????????
        ??????????????????????????????maxWorkspaceSize????????????????????????128MB = 1ul << 27
    **/
    bool compile(
        Mode mode,
        unsigned int maxBatchSize,
        const ModelSource &source,
        const CompileOutput &saveto,
        const std::vector<InputDims> inputsDimsSetup = {},
        Int8Process int8process = nullptr,
        const std::string &int8ImageDirectory = "",
        const std::string &int8EntropyCalibratorFile = "",
        const size_t maxWorkspaceSize = 1ul << 30 // 1ul << 30 = 1GB
    );
};

#endif // TRT_BUILDER_HPP