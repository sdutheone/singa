#include <glog/logging.h>
#include <thread>
#include <memory>
#include "worker/worker.h"
#include "proto/model.pb.h"
#include "utils/cluster.h"
using std::thread;
namespace singa {
Worker::Worker(shared_ptr<Cluster> cluster){
  cluster_=cluster;
}

void Worker::Start(ModelProto model){
  LOG(ERROR)<<"Worker on "<<cluster_->hostname()<<" is starting...";
  // update proto_, all groups will do the training, but only the first group
  // conduts test/validation. Hence, the training mini-batch is partitioned
  // onto all groups.
  if(cluster_->groupid()==0){
    int ngroups=cluster_->ngroups();
    model.set_validation_frequency(
        model.validation_frequency()/ngroups);
    model.set_test_frequency(model.test_frequency()/ngroups);
  }
  Setup(0, model); //setup main executor

  train_net_=SetupNeuralNet(model.neuralnet(), model.prefetch(), kTrain);
  test_net_=SetupNeuralNet(model.neuralnet(), model.prefetch(), kTest);
  if(test_net_!=nullptr)
    test_net_->ShareWeights(train_net_);
  validation_net_=SetupNeuralNet(model.neuralnet(),model.prefetch(), kValidation);
  if(validation_net_!=nullptr)
    validation_net_->ShareWeights(train_net_);

  ParamManager pm(train_net_, model.updater());
  if(cluster_->groupid()==0){
    pm.InitParams(); //init local params
  }else{
    pm.GetParamsFromServers(); // will be blocked until recv all parameters.
  }

  int nthreads=cluster_->nthreads_per_procs();
  vector<Executor*> executors(nthreads-1);
  vector<thread> threads;
  for(size_t i=1;i<executors.size();i++){
    executors[i]=new Executor(i, model, cluster_, train_net_);
    threads.push_back(thread(&Executor::Run, executors[i], &pm, 0));
  }
  if(cluster_->groupid()==0){
    Performance perf(train_net_);
    for(int i=0;i<model.warmup_steps();i++){
      RunOneBatch(i, &perf);
      pm.Update(i, local_threadid_);
    }
    pm.SendParamsToServers();
  }
  Run(&pm, model.warmup_steps());
  for(auto& th: threads)
    th.join();
  for(size_t i=1;i<executors.size();i++){
    delete executors[i];
  }
  //LOG(ERROR)<<"Worker on "<<hostname_<< " is shutting down";
}

void Worker::Resume() {
  // TODO implement resume from snapshot
}

shared_ptr<NeuralNet> Worker::SetupNeuralNet(const NetProto& np, bool prefetch,
    Phase phase){
  // only consider training phase now.
  // TODO reset the proto to config test and valdiation neuralnet.
  // diff nets should have diff layer objects,
  // but share parameter objects (by ptr).
  shared_ptr<NeuralNet> net(new NeuralNet(np));
  for(auto& layer: net->parserlayers()){
    layer->set_prefetch(prefetch);
  }
  for(auto& layer: net->datalayers()){
    layer->set_prefetch(prefetch);
  }
  return net;
}

void Worker::Run(ParamManager* pm, const int start_step){
  step_=start_step;
  Performance perf(train_net_);
  while(!StopNow(step_)){
    RunOneBatch(step_, &perf);
    pm->Update(step_, local_threadid_);
    // communicate with others
    step_++;
  }
}


/**************************Executor***********************************/
Executor::Executor(int local_threadid, const ModelProto& model,
    shared_ptr<Cluster> cluster,
    shared_ptr<NeuralNet> train_net,
    shared_ptr<NeuralNet> test_net,
    shared_ptr<NeuralNet> validation_net):
      cluster_(cluster),
      train_net_(train_net),
      test_net_(test_net),
      validation_net_(validation_net){
        tForward_=tBackward_=tSyncData_=tSyncParam_=0;
        Setup(local_threadid, model);
      }

void Executor::Setup(int local_threadid, const ModelProto& model){
  modelproto_=model;
  local_threadid_=local_threadid;
  if(model.prefetch()){
    auto cluster=Cluster::Get();
    for(auto& layer: train_net_->datalayers()){
      int locid=layer->locationid();
      if(cluster->group_threadid(local_threadid_)==locid)
        localDataLayers_.push_back(layer);
    }
    if(localDataLayers_.size())
      prefetch_thread_=std::thread(Executor::PrefetchData,
          std::ref(localDataLayers_), true,1);
  }
  int gthreadid=cluster_->group_threadid(local_threadid);
  // TODO create a message queue and network thread for send and recv
  for(auto& layer: train_net_->layers()){ //TODO check with PS
    if(layer->locationid()==gthreadid){
      int pushloc=-1;
      if(layer->is_bridgesrclayer())
        pushloc=layer->dstlayers()[0]->locationid();
      else if(layer->is_bridgedstlayer())
        pushloc=layer->srclayers()[0]->locationid();
      if(pushloc!=-1&&push_.find(pushloc)==push_.end()){
        string endpoint="@tcp://*:"+cluster_->pull_port(local_threadid);
        pull_=zsock_new_pull(endpoint.c_str());
        string pushaddr=cluster_->addr(cluster_->global_procsid(pushloc));
        endpoint=">tcp://"+pushaddr
            +":"+cluster_->pull_port(pushloc%cluster_->nthreads_per_procs());
        push_[pushloc]= zsock_new_push(endpoint.c_str());
      }
    }
  }
}

Executor::~Executor(){
  if(prefetch_thread_.joinable())
    prefetch_thread_.join();
}

void Executor::PrefetchData(const vector<DataLayer*>& datalayers,  bool training,
    int steps){
  if(datalayers.size()==0)
    return;
  for(int i=0;i<steps;i++){
    for(auto& layer: datalayers){
      layer->ComputeFeature(training);
      CHECK(layer->dstlayers()[0]->is_parserlayer());
      auto parserlayer=static_cast<ParserLayer*>(layer->dstlayers()[0].get());
      parserlayer->Prefetching(training);
    }
  }
}

void Executor::Run(ParamManager*pm, int step){
  step_=step;
  while(!StopNow(step_)){
    RunOneBatch(step_);
    pm->Update(step_, local_threadid_);
    step_++;
  }
}

void Executor::RunOneBatch(int step, Performance* perf){
  ticks_++;
  // Test will call Pull which updates the sync time
  // Hence we store the sync time, and restore it later
  float tSyncData=tSyncData_, tSyncParam=tSyncParam_;
  if(ValidateNow(step)){
    LOG(INFO)<<"Validation at step "<<step;
    Test(validation_net_, modelproto_.validation_steps(), perf==nullptr);
  }
  if(TestNow(step)){
    LOG(INFO)<<"Test at step "<<step;
    Test(test_net_, modelproto_.validation_steps(), perf==nullptr);
  }
  tSyncData_=tSyncData;
  tSyncParam_=tSyncParam;

  TrainOneBatch(step);
  if(perf!=nullptr){
    perf->Update();
    if(DisplayNow(step)){
      LOG(INFO)<<"Training at step "<<step;
      LOG(INFO)<<perf->ToString();
      LOG(INFO)<<TimerInfo();
      DLOG(INFO)<<train_net_->DebugInfo();
      perf->Reset();
    }
  }
}

void Executor::Pull(zsock_t* pull, shared_ptr<NeuralNet> net){
  int type;
  char *name;
  int64_t tick=zclock_mono();
  zframe_t* frame=zframe_new_empty();

  zsock_recv(pull_, "isf", &type, &name, &frame);
  if(type==kDataFrame){
    auto* dst=static_cast<BridgeDstLayer*>(
        net->name2layer(string(name)).get());
    memcpy(dst->mutable_data()->mutable_cpu_data(), zframe_data(frame),
        zframe_size(frame));
    dst->set_ready(true);
  }else if(type==kGradFrame){
    auto* src=static_cast<BridgeSrcLayer*>(net->name2layer(string(name)).get());
    memcpy(src->mutable_grad()->mutable_cpu_data(), zframe_data(frame),
        zframe_size(frame));
    src->set_ready(true);
  }
  zframe_destroy(&frame);
  delete name;
  tSyncData_+=zclock_mono()-tick;
}

void Executor::Forward(shared_ptr<NeuralNet> net, bool training){
  auto& layers=net->layers();
  for(auto& layer: layers){
    if(layer->is_bridgedstlayer()){
      auto* dst=static_cast<BridgeDstLayer*>(layer.get());
      while(!dst->ready())
        Pull(pull_, train_net_);
    }
    layer->ComputeFeature(training);
    if(layer->is_bridgesrclayer()){
      zframe_t* frame=zframe_new(layer->data().cpu_data(),
          layer->data().count()*sizeof(float));
      zsock_send(push_[layer->locationid()], "isf",
          kDataFrame, layer->dstlayers()[0]->name().c_str(), frame);
      zframe_destroy(&frame);
    }
  }
}

void Executor::Backward(shared_ptr<NeuralNet> net){
  auto& layers=net->layers();
  for (auto it = layers.rbegin(); it != layers.rend(); it++){
    shared_ptr<Layer> layer=*it;
    if(layer->is_bridgesrclayer()){
      auto* src=static_cast<BridgeSrcLayer*>(layer.get());
      while(!src->ready())
        Pull(pull_, train_net_);
    }
    layer->ComputeGradient();
    if(layer->is_bridgedstlayer()){
      zframe_t* frame=zframe_new(layer->grad().cpu_data(),
          layer->data().count()*sizeof(float));
      zsock_send(push_[layer->locationid()], "isf",
          kGradFrame, layer->srclayers()[0]->name().c_str(), frame);
      zframe_destroy(&frame);
    }
  }
}

void Executor::TrainOneBatch(int step){
  int64_t tick=zclock_mono();
  if(prefetch_thread_.joinable()){
      prefetch_thread_.join();
      prefetch_thread_=std::thread(Executor::PrefetchData,
          std::ref(localDataLayers_), false,1);
  }
  Forward(train_net_, true);
  tForward_+=zclock_mono()-tick;
  tick=zclock_mono();
  Backward(train_net_);
  tBackward_+=zclock_mono()-tick;
}

void Executor::Test(shared_ptr<NeuralNet> net, int nsteps, bool disperf){
  std::thread prefetch;
  vector<DataLayer*> localDataLayers;
  auto cluster=Cluster::Get();
  for(auto& layer: net->datalayers()){
    int locid=layer->locationid();
    if(cluster->group_threadid(local_threadid_)==locid)
      localDataLayers.push_back(layer);
  }
  if(modelproto_.prefetch())
   prefetch=std::thread(Executor::PrefetchData,  std::ref(localDataLayers), false,1);
  Performance perf(net);
  for(int b=0;b<nsteps;b++){
    if(prefetch.joinable()){
      prefetch.join();
      prefetch=std::thread(Executor::PrefetchData, std::ref(localDataLayers), false,1);
    }
    Forward(net, false);
    if(disperf)
      perf.Update();
  }
  if(prefetch.joinable())
    prefetch.join();
  if(disperf)
    LOG(INFO)<<perf.ToString();
}
/*********************Implementation for Performance class*******************/
Performance::Performance(shared_ptr<NeuralNet> net):net_(net), counter_(0){
  for(auto& layer: net->losslayers()){
    name_.push_back(layer->name());
    metric_.push_back(vector<float>{});
    metric_.back().resize(layer->metric().count(),0.f);
  }
}

void Performance::Update(){
  const auto& losslayers=net_->losslayers();
  for(size_t i=0;i<losslayers.size();i++){
    const float * ptr=losslayers[i]->metric().cpu_data();
    vector<float>& m=metric_.at(i);
    for(int j=0;j<losslayers[i]->metric().count();j++)
      m[j]+=ptr[j];
  }
  counter_++;
}

void Performance::Reset(){
  for(auto& m: metric_)
    for(auto& x: m)
      x=0.f;
  counter_=0;
}

string Performance::ToString(){
  string disp="";
  for(size_t i=0;i<metric_.size();i++){
    disp+="Output from "+name_[i]+" layer ";
    vector<float> m=metric_.at(i);
    for(size_t j=0;j<m.size();j++)
        disp+=std::to_string(j)+" : "+std::to_string(m[j]/counter_)+"\t";
    disp+="\n";
  }
  return disp;
}

}  // namespace singa
