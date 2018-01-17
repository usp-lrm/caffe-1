#include <opencv2/core/core.hpp>

#include <fstream>  // NOLINT(readability/streams)
#include <iostream>  // NOLINT(readability/streams)
#include <string>
#include <utility>

#include "caffe/layers/dense_image_data_layer.hpp"
#include "caffe/util/benchmark.hpp"
#include "caffe/util/io.hpp"
#include "caffe/util/math_functions.hpp"
#include "caffe/util/rng.hpp"

namespace caffe {

template <typename Dtype>
DenseImageDataLayer<Dtype>::~DenseImageDataLayer<Dtype>() {
  this->StopInternalThread();
}


template <typename Dtype>
void DenseImageDataLayer<Dtype>::DataLayerSetUp(const vector<Blob<Dtype>*>& bottom,
      const vector<Blob<Dtype>*>& top) {
  const int new_height = this->layer_param_.dense_image_data_param().new_height();
  const int new_width  = this->layer_param_.dense_image_data_param().new_width();
  const bool is_color  = this->layer_param_.dense_image_data_param().is_color();
  string root_folder = this->layer_param_.dense_image_data_param().root_folder();

  CHECK((new_height == 0 && new_width == 0) ||
      (new_height > 0 && new_width > 0)) << "Current implementation requires "
      "new_height and new_width to be set at the same time.";
  // Read the file with filenames and labels
  const string& source = this->layer_param_.dense_image_data_param().source();
  LOG(INFO) << "Opening file " << source;
  std::ifstream infile(source.c_str());
  string filename;
  string label_filename;
  while (infile >> filename >> label_filename) {
    lines_.push_back(std::make_pair(filename, label_filename));
  }

  CHECK(!lines_.empty()) << "File is empty";

  if (this->layer_param_.dense_image_data_param().shuffle()) {
    // randomly shuffle data
    LOG(INFO) << "Shuffling data";
    const unsigned int prefetch_rng_seed = caffe_rng_rand();
    prefetch_rng_.reset(new Caffe::RNG(prefetch_rng_seed));
    ShuffleImages();
  } else {
    if (this->phase_ == TRAIN &&
        this->layer_param_.image_data_param().rand_skip() == 0){
      LOG(WARNING) << "Shuffling or skipping recommended for multi-GPU";
    }
  }
  LOG(INFO) << "A total of " << lines_.size() << " images.";

  lines_id_ = 0;
  // Check if we would need to randomly skip a few data points
  if (this->layer_param_.dense_image_data_param().rand_skip()) {
    unsigned int skip = caffe_rng_rand() %
        this->layer_param_.dense_image_data_param().rand_skip();
    LOG(INFO) << "Skipping first " << skip << " data points.";
    CHECK_GT(lines_.size(), skip) << "Not enough points to skip";
    lines_id_ = skip;
  }

  // Read an image, and use it to initialize the top blobs.
  cv::Mat cv_img = ReadImageToCVMat(root_folder + lines_[lines_id_].first,
                                    new_height, new_width, is_color);
  CHECK(cv_img.data) << "Could not load " << lines_[lines_id_].first;

  // Use data_transformer to infer the expected blob shape from a cv_image.
  vector<int> top_shape = this->data_transformer_->InferBlobShape(cv_img);
  this->transformed_data_.Reshape(top_shape);

  // Reshape prefetch_data and top[0] according to the batch_size.
  const int batch_size = this->layer_param_.dense_image_data_param().batch_size();
  CHECK_GT(batch_size, 0) << "Positive batch size required";
  top_shape[0] = batch_size;
  for (int i = 0; i < this->PREFETCH_COUNT; ++i) {
    this->prefetch_[i].data_.Reshape(top_shape);
  }
  top[0]->Reshape(top_shape);

  LOG(INFO) << "input data size: " << top[0]->num() << ","
      << top[0]->channels() << "," << top[0]->height() << ","
      << top[0]->width();


  // sanity check label image
  cv::Mat cv_lab = ReadImageToCVMat(root_folder + lines_[lines_id_].second,
                                    new_height, new_width, false);
  CHECK(cv_lab.data) << "Could not load " << lines_[lines_id_].second;
  CHECK(cv_lab.channels() == 1) << "Can only handle grayscale label images";
  CHECK(cv_lab.rows == cv_img.rows && cv_lab.cols == cv_img.cols) << "Input and label "
      << "image heights and widths must match";

  // Use data_transformer to infer the expected blob shape from a cv image.
  vector<int> label_shape = this->data_transformer_->InferBlobShape(cv_lab);
  this->transformed_label_.Reshape(label_shape);

  label_shape[0] = batch_size;
  for (int i = 0; i < this->PREFETCH_COUNT; ++i) {
    this->prefetch_[i].label_.Reshape(label_shape);
  }
  top[1]->Reshape(label_shape);

  LOG(INFO) << "label data size: " << top[1]->num() << ","
      << top[1]->channels() << "," << top[1]->height() << ","
      << top[1]->width();
}

template <typename Dtype>
void DenseImageDataLayer<Dtype>::ShuffleImages() {
  caffe::rng_t* prefetch_rng =
      static_cast<caffe::rng_t*>(prefetch_rng_->generator());
  shuffle(lines_.begin(), lines_.end(), prefetch_rng);
}



// This function is called on prefetch thread
template <typename Dtype>
void DenseImageDataLayer<Dtype>::load_batch(Batch<Dtype>* batch) {
  CPUTimer batch_timer;
  batch_timer.Start();
  double read_time = 0;
  double trans_time = 0;
  CPUTimer timer;
  CHECK(batch->data_.count());
  CHECK(this->transformed_data_.count());
  CHECK(this->transformed_label_.count());
  DenseImageDataParameter dense_image_data_param = this->layer_param_.dense_image_data_param();
  const int batch_size = dense_image_data_param.batch_size();
  const int new_height = dense_image_data_param.new_height();
  const int new_width = dense_image_data_param.new_width();
  const bool is_color = dense_image_data_param.is_color();
  string root_folder = dense_image_data_param.root_folder();


  // Reshape according to the first image of each batch
  // on single input batches allows for inputs of varying dimension.
  cv::Mat cv_img = ReadImageToCVMat(root_folder + lines_[lines_id_].first,
      new_height, new_width, is_color);
  CHECK(cv_img.data) << "Could not load " << lines_[lines_id_].first;
  // Use data_transformer to infer the expected blob shape from a cv_img.
  vector<int> top_shape = this->data_transformer_->InferBlobShape(cv_img);
  this->transformed_data_.Reshape(top_shape);
  // Reshape batch according to the batch_size.
  top_shape[0] = batch_size;
  batch->data_.Reshape(top_shape);

  cv::Mat cv_lab = ReadImageToCVMat(root_folder + lines_[lines_id_].second,
                                    new_height, new_width, false);
  CHECK(cv_lab.data) << "Could not load " << lines_[lines_id_].second;
  CHECK(cv_lab.channels() == 1) << "Can only handle grayscale label images";
  CHECK(cv_lab.rows == cv_img.rows && cv_lab.cols == cv_img.cols) << "Input and label "
      << "image heights and widths must match";
  // Use data_transformer to infer the expected blob shape from a cv image.
  vector<int> label_shape = this->data_transformer_->InferBlobShape(cv_lab);
  this->transformed_label_.Reshape(label_shape);
  label_shape[0] = batch_size;
  batch->label_.Reshape(label_shape);

  Dtype* prefetch_data = batch->data_.mutable_cpu_data();
  Dtype* prefetch_label = batch->label_.mutable_cpu_data();

  // datum scales
  const int lines_size = lines_.size();
  for (int item_id = 0; item_id < batch_size; ++item_id) {
    // get a blob
    timer.Start();
    CHECK_GT(lines_size, lines_id_);
    cv::Mat cv_img = ReadImageToCVMat(root_folder + lines_[lines_id_].first,
        new_height, new_width, is_color);
    CHECK(cv_img.data) << "Could not load " << lines_[lines_id_].first;
    cv::Mat cv_lab = ReadImageToCVMat(root_folder + lines_[lines_id_].second,
        new_height, new_width, false);
    CHECK(cv_lab.data) << "Could not load " << lines_[lines_id_].second;
    read_time += timer.MicroSeconds();
    timer.Start();
    // Apply transformations (mirror, crop...) to the image
    int offset = batch->data_.offset(item_id);
    this->transformed_data_.set_cpu_data(prefetch_data + offset);
    this->data_transformer_->Transform(cv_img, &(this->transformed_data_));
    // transform label the same way
    int label_offset = batch->label_.offset(item_id);
    this->transformed_label_.set_cpu_data(prefetch_label + label_offset);
    this->data_transformer_->Transform(cv_lab, &(this->transformed_label_));
    CHECK(!this->layer_param_.transform_param().mirror() &&
        this->layer_param_.transform_param().crop_size() == 0)
        << "FIXME: Any stochastic transformation will break layer due to "
        << "the need to transform input and label images in the same way";
    trans_time += timer.MicroSeconds();
//    prefetch_label[item_id] = lines_[lines_id_].second;
    // go to the next iter
    lines_id_++;
    if (lines_id_ >= lines_size) {
      // We have reached the end. Restart from the first.
      DLOG(INFO) << "Restarting data prefetching from start.";
      lines_id_ = 0;
      if (this->layer_param_.dense_image_data_param().shuffle()) {
        ShuffleImages();
      }
    }
  }
  batch_timer.Stop();
  DLOG(INFO) << "Prefetch batch: " << batch_timer.MilliSeconds() << " ms.";
  DLOG(INFO) << "     Read time: " << read_time / 1000 << " ms.";
  DLOG(INFO) << "Transform time: " << trans_time / 1000 << " ms.";
}

INSTANTIATE_CLASS(DenseImageDataLayer);
REGISTER_LAYER_CLASS(DenseImageData);

}  // namespace caffe
