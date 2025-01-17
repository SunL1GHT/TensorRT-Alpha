#include"yolo_nas.h"
#include"decode_yolo_nas.h"

YOLO_NAS::YOLO_NAS(const utils::InitParameter& param) :yolo::YOLO(param)
{
    m_resize_shape = cv::Size(636, 636);
    m_input_resize_padding_device = nullptr;
    checkRuntime(cudaMalloc(&m_input_resize_padding_device, param.batch_size * 3 * m_param.dst_h * m_param.dst_w * sizeof(float)));
}

YOLO_NAS::~YOLO_NAS()
{
    checkRuntime(cudaFree(m_input_resize_padding_device));
}

bool YOLO_NAS::init(const std::vector<unsigned char>& trtFile)
{
    if (trtFile.empty())
    {
        return false;
    }
    std::unique_ptr<nvinfer1::IRuntime> runtime =
        std::unique_ptr<nvinfer1::IRuntime>(nvinfer1::createInferRuntime(sample::gLogger.getTRTLogger()));
    if (runtime == nullptr)
    {
        return false;
    }
    this->m_engine = std::unique_ptr<nvinfer1::ICudaEngine>(runtime->deserializeCudaEngine(trtFile.data(), trtFile.size()));
    if (this->m_engine == nullptr)
    {
        return false;
    }
    this->m_context = std::unique_ptr<nvinfer1::IExecutionContext>(this->m_engine->createExecutionContext());
    if (this->m_context == nullptr)
    {
        return false;
    }
    if (m_param.dynamic_batch)
    {
        this->m_context->setBindingDimensions(0, nvinfer1::Dims4(m_param.batch_size, 3, m_param.dst_h, m_param.dst_w));
    }
    m_output_dims = this->m_context->getBindingDimensions(1);
    m_total_objects = m_output_dims.d[1];
    assert(m_param.batch_size <= m_output_dims.d[0]);
    m_output_area = 1; 
    for (int i = 1; i < m_output_dims.nbDims; i++)
    {
        if (m_output_dims.d[i] != 0)
        {
            m_output_area *= m_output_dims.d[i];
        }
    }
    checkRuntime(cudaMalloc(&m_output_src_device, m_param.batch_size * m_output_area * sizeof(float)));
    float a = float(m_resize_shape.height) / m_param.src_h;
    float b = float(m_resize_shape.width) / m_param.src_w;
    float scale = a < b ? a : b;
    cv::Mat src2dst = (cv::Mat_<float>(2, 3) << scale, 0.f, (-scale * m_param.src_w + m_resize_shape.width + scale - 1) * 0.5,
        0.f, scale, (-scale * m_param.src_h + m_resize_shape.height + scale - 1) * 0.5);
    cv::Mat dst2src = cv::Mat::zeros(2, 3, CV_32FC1);
    cv::invertAffineTransform(src2dst, dst2src);
    int pad_height = m_param.dst_h - m_resize_shape.height;
    int pad_width = m_param.dst_w - m_resize_shape.width;
    m_pad_top = pad_height / 2;
    m_pad_left = pad_width / 2;

    m_dst2src.v0 = dst2src.ptr<float>(0)[0];
    m_dst2src.v1 = dst2src.ptr<float>(0)[1];
    m_dst2src.v2 = dst2src.ptr<float>(0)[2];
    m_dst2src.v3 = dst2src.ptr<float>(1)[0];
    m_dst2src.v4 = dst2src.ptr<float>(1)[1];
    m_dst2src.v5 = dst2src.ptr<float>(1)[2];
    return true;
}

void YOLO_NAS::preprocess(const std::vector<cv::Mat>& imgsBatch)
{
    resizeDevice(m_param.batch_size, m_input_src_device, m_param.src_w, m_param.src_h,
        m_input_resize_device, m_resize_shape.width, m_resize_shape.height, 114, m_dst2src);
    copyWithPaddingDevice(m_param.batch_size, m_input_resize_device, m_resize_shape.width, m_resize_shape.height,
        m_input_resize_padding_device, m_param.dst_w, m_param.dst_h, 114.f, m_pad_top, m_pad_left);
    bgr2rgbDevice(m_param.batch_size, m_input_resize_padding_device, m_param.dst_w, m_param.dst_h,
        m_input_rgb_device, m_param.dst_w, m_param.dst_h);
    normDevice(m_param.batch_size, m_input_rgb_device, m_param.dst_w, m_param.dst_h,
        m_input_norm_device, m_param.dst_w, m_param.dst_h, m_param);
    hwc2chwDevice(m_param.batch_size, m_input_norm_device, m_param.dst_w, m_param.dst_h,
        m_input_hwc_device, m_param.dst_w, m_param.dst_h);
}
void YOLO_NAS::postprocess(const std::vector<cv::Mat>& imgsBatch)
{
    yolo_nas::decodeDevice(m_param, m_output_src_device, 4 + m_param.num_class, m_total_objects, m_output_area,
        m_output_objects_device, m_output_objects_width, m_param.topK);
    nmsDeviceV1(m_param, m_output_objects_device, m_output_objects_width, m_param.topK, m_param.topK * m_output_objects_width + 1);
    //nmsDeviceV2(m_param, m_output_objects_device, m_output_objects_width, m_param.topK, m_param.topK * m_output_objects_width + 1, m_output_idx_device, m_output_conf_device);
    checkRuntime(cudaMemcpy(m_output_objects_host, m_output_objects_device, m_param.batch_size * sizeof(float) * (1 + 7 * m_param.topK), cudaMemcpyDeviceToHost));
    for (size_t bi = 0; bi < imgsBatch.size(); bi++)
    {
        int num_boxes = std::min((int)(m_output_objects_host + bi * (m_param.topK * m_output_objects_width + 1))[0], m_param.topK);
        for (size_t i = 0; i < num_boxes; i++)
        {
            float* ptr = m_output_objects_host + bi * (m_param.topK * m_output_objects_width + 1) + m_output_objects_width * i + 1;
            int keep_flag = ptr[6];
            if (keep_flag)
            {            
                ptr[0] -= m_pad_left;
                ptr[1] -= m_pad_top;
                ptr[2] -= m_pad_left;
                ptr[3] -= m_pad_top;
                float x_lt = m_dst2src.v0 * ptr[0] + m_dst2src.v1 * ptr[1] + m_dst2src.v2; 
                float y_lt = m_dst2src.v3 * ptr[0] + m_dst2src.v4 * ptr[1] + m_dst2src.v5;
                float x_rb = m_dst2src.v0 * ptr[2] + m_dst2src.v1 * ptr[3] + m_dst2src.v2;
                float y_rb = m_dst2src.v3 * ptr[2] + m_dst2src.v4 * ptr[3] + m_dst2src.v5;
                m_objectss[bi].emplace_back(x_lt, y_lt, x_rb, y_rb, ptr[4], (int)ptr[5]);
            }
        }
    }
}