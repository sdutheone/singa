// Copyright © 2014 Wei Wang. All Rights Reserved.
// 2014-07-14 14:28

#include <glog/logging.h>
#include <leveldb/db.h>
#include <lmdb.h>
#include <mpi.h>
#include <vector>
#include <typeinfo>
#include "utils/proto_helper.h"
#include "proto/model.pb.h"
#include "net/solver.h"
#include "da/gary.h"
#include "utils/debug.h"


DECLARE_string(db_backend);
namespace lapis {
Phase Solver::phase=Phase::kTrain;
Solver::Solver(const SolverProto &proto) {
  //! if step_>0, then the trainer is restored from a checkpoint
  step_ = proto.checkpoint_step();
  checkpoint_after_steps_ = proto.checkpoint_after_steps();
  checkpoint_every_steps_ = proto.checkpoint_every_steps();
  //! last checkpoint step
  checkpoint_step_ = proto.checkpoint_step();
  display_after_steps_ = proto.display_after_steps();
  display_every_steps_ = proto.display_every_steps();
  validation_after_steps_ = proto.validation_after_steps();
  validation_every_steps_ = proto.validation_every_steps();
  test_after_steps_ = proto.test_after_steps();
  test_every_steps_ = proto.test_every_steps();

  train_steps_=proto.train_steps();
  test_steps_=proto.test_steps();
  validation_steps_=proto.validation_steps();

  sgd_=proto.sgd();
  context_=GlobalContext::Get();
  mpi_=NetworkThread::Get();
}

float Solver::UpdateHyperParam(int step, SGDValue::ChangeProto change,
    int change_steps, float a, float b) {
  float ret = 0., r = 0.;
  switch (change) {
    case SGDValue::kFixed:
      ret = a;
      break;
    case SGDValue::kLinear:
      // a is init, b is the final
      r = step * 1.0  / change_steps;
      ret = (1.0 - r) * a + r * b;
      break;
    case SGDValue::kExponential:
      // a is init, b is the final, from convnet
      CHECK_EQ(a, 2 * b) << "final value should be the half";
      ret = a / pow(2, step * 1. / change_steps);
      break;
    case SGDValue::kInverse_t:
      // a is init, b is the final, from convnet
      CHECK_EQ(a, 2 * b) << "final value should be the half";
      ret = a / (1. + step * 1. / b);
      break;
    case SGDValue::kStep:
      // a is the base learning rate, b is gamma, from caffe
      // notice it is step/change_steps, not step*1.0/change_steps
      ret = a * pow(b, step / change_steps);
      break;
    default:
      LOG(ERROR) << "Wrong hyper-parameter update method";
  }
  return ret;
}

void Solver::LocalUpdate(Param* param, int step) {
  float lr=UpdateHyperParam(step, sgd_.learning_rate_change(),
      sgd_.learning_rate_change_steps(),sgd_.base_learning_rate(), sgd_.gamma());
  DAry* history=param->mutable_history();
  const DAry& grad=param->grad();
  const DAry& data=param->data();
  int len=data.local_size();
  CHECK_EQ(len, grad.local_size());
  CHECK_EQ(len, history->local_size());
  lr=lr*param->learning_rate_multiplier();
  float w=sgd_.weight_decay()*param->weight_decay_multiplier();
  // hist=hist-lr*grad
  DAry::arymath().madd(history->dptr(), lr, grad.dptr(), history->dptr(), len);
  // hist=hist-lr*weight*param
  if(w>0)
    DAry::arymath().madd(history->dptr(), lr*w, data.dptr(), history->dptr(), len);

  // param+=history/n, /data->n_update()
  DAry::arymath().sub(data.dptr(), data.dptr(), history->dptr(), len);
  // hist=hist*mom
  DAry::arymath().mul(history->dptr(), sgd_.momentum(), history->dptr(), len);
}


void Solver::Setup(TableDelegate* delegate, const DataProto& dp, const NetProto& np){
  net_=new Net(np);
  net_->Setup();
  auto params=net_->params();
  auto grp_rank=context_->worker_id();
  delegate_=delegate;
  delegate_->SplitParams(params, grp_rank);
  string shard_folder=GlobalContext::Get()->shard_folder();
  train_shard_=shard_folder+"/"+dp.train_data().name()+"-"+FLAGS_db_backend;
  val_shard_=shard_folder+"/"+dp.validation_data().name()+"-"+FLAGS_db_backend;
  test_shard_=shard_folder+"/"+dp.test_data().name()+"-"+FLAGS_db_backend;
}

void Solver::InitParams(){
  for(auto* param: net_->params()){
    param->Fill();
    LOG(INFO)<<"param "<<param->name()<<" "<<param->data().Norm1();
  }
  for(auto* param: net_->params())
    if(!param->partition()||GlobalContext::Get()->num_groups()>1)
      delegate_->Put(param);
}

Solver::~Solver() {
  delete net_;
}
void Solver::ToProto(SolverProto *proto) {
  proto->set_checkpoint_after_steps(checkpoint_after_steps_);
  proto->set_checkpoint_every_steps(checkpoint_every_steps_);
  proto->set_checkpoint_step(checkpoint_step_);

  proto->set_display_after_steps(display_after_steps_);
  proto->set_display_every_steps(display_every_steps_);

  proto->set_validation_after_steps(validation_after_steps_);
  proto->set_validation_every_steps(validation_every_steps_);

  proto->set_test_after_steps(test_after_steps_);
  proto->set_test_every_steps(test_every_steps_);
}
Prefetcher::Prefetcher(string path, Net* _net) {
  net=_net;
  // Initialize DB
  if(FLAGS_db_backend=="leveldb"){
    leveldb::Options options;
    options.create_if_missing = false;
    options.max_open_files = 100;
    LOG(INFO) << "Opening leveldb " << path;
    leveldb::Status status = leveldb::DB::Open( options, path, &db);
    CHECK(status.ok()) << "Failed to open leveldb "
      << path << std::endl << status.ToString();
    leveldb::ReadOptions readopt;
    readopt.fill_cache=false;
    iter=db->NewIterator(readopt);
    iter->SeekToFirst();
  }else if(FLAGS_db_backend=="lmdb"){
    CHECK_EQ(mdb_env_create(&mdb_env), MDB_SUCCESS) << "mdb_env_create failed";
    CHECK_EQ(mdb_env_set_mapsize(mdb_env, 1099511627776), MDB_SUCCESS);
    CHECK_EQ(mdb_env_open(mdb_env, path.c_str(),
            MDB_RDONLY|MDB_NOTLS, 0664), MDB_SUCCESS) << "mdb_env_open failed";
    CHECK_EQ(mdb_txn_begin(mdb_env, NULL, MDB_RDONLY, &mdb_txn), MDB_SUCCESS)
      << "mdb_txn_begin failed";
    CHECK_EQ(mdb_open(mdb_txn, NULL, 0, &mdb_dbi), MDB_SUCCESS)
      << "mdb_open failed";
    CHECK_EQ(mdb_cursor_open(mdb_txn, mdb_dbi, &mdb_cursor), MDB_SUCCESS)
      << "mdb_cursor_open failed";
    LOG(INFO) << "Opening lmdb " <<path;
    CHECK_EQ(mdb_cursor_get(mdb_cursor, &mdb_key, &mdb_value, MDB_FIRST),
        MDB_SUCCESS) << "mdb_cursor_get failed";
  }else{
    LOG(FATAL) << "Unknown database backend";
  }
}

void Prefetcher::Free(){
  if(FLAGS_db_backend=="leveldb"){
    delete db;
    delete iter;
  }else{
    mdb_cursor_close(mdb_cursor);
    mdb_close(mdb_env, mdb_dbi);
    mdb_txn_abort(mdb_txn);
    mdb_env_close(mdb_env);
  }
}


void Prefetcher::NextIterator(){
  if(FLAGS_db_backend=="leveldb"){
    iter->Next();
    if(!iter->Valid()){
      LOG(INFO)<<"reset to start leveldb";
      iter->SeekToFirst();
    }
  }else{
    if( mdb_cursor_get(mdb_cursor, &mdb_key,
          &mdb_value, MDB_NEXT)!= MDB_SUCCESS){
      LOG(INFO)<<"reset to start lmdb";
      CHECK_EQ(mdb_cursor_get(mdb_cursor, &mdb_key,
          &mdb_value, MDB_FIRST), MDB_SUCCESS);
    }
  }
}

void Prefetcher::ReadRecord(Record* record){
  if(FLAGS_db_backend=="leveldb"){
    record->ParseFromString(iter->value().ToString());
  }else{
    CHECK_EQ(mdb_cursor_get(mdb_cursor, &mdb_key,
          &mdb_value, MDB_GET_CURRENT), MDB_SUCCESS);
    record->ParseFromArray(mdb_value.mv_data, mdb_value.mv_size);
  }
}
void debug_mem(string prefix){
  char buf[1024];
  sprintf(buf, "%30s, %12lu", prefix.c_str(), getCurrentRSS());
  LOG(INFO)<<string(buf);
}
void* PrefetchData(void* context){
  Prefetcher *prefetcher=static_cast<Prefetcher*>(context);
  Net* net=prefetcher->net;
  const DAry& input= net->input_layer(0)->GetData(nullptr);
  Range nrng=input.IndexRange(0);
  /*
  for(int n=0;n<nrng.first;n++)
    prefetcher->NextIterator();
    */
  Record record;
  for(int n=0;n<nrng.second-nrng.first;++n){
    prefetcher->ReadRecord(&record);
    for(auto* layer:net->input_layer())
      layer->AddInputRecord(record);
    prefetcher->NextIterator();
  }
  /*
  for(int n=0;n<input.shape(0)-nrng.second;n++)
    prefetcher->NextIterator();
    */
}

void Solver::Test(){
  Performance perf;
  while(!HasFinished()){
    if(ValidateNow()){
      perf=Test(Phase::kValidation);
      ReportPerformance("Validation", perf);
    }
    if(TestNow()){
      perf=Test(Phase::kTest);
      ReportPerformance("Test", perf);
    }
    IncStep();
    sleep(1);
  }
}
Performance Solver::Test(const Phase& phase){
  string shard;
  int nsteps;
  if(phase==Phase::kValidation){
    shard=val_shard_;
    nsteps=validation_steps_;
  }
  else if(phase==Phase::kTest){
    shard=test_shard_;
    nsteps=test_steps_;
  }
  else
    LOG(ERROR)<<"Phase must be kValidation or kTest";
  // fetch params once

  Prefetcher prefetcher(shard, net_);
  pthread_create(&prefetch_thread_, NULL, &PrefetchData, &prefetcher);
  Solver::phase=phase;
  Performance perf;
  for(int b=0;b<nsteps;b++){
    pthread_join(prefetch_thread_, NULL);
    for(auto* layer:net_->input_layer())
      layer->SetInputData(nullptr);
    pthread_create(&prefetch_thread_, NULL, &PrefetchData, &prefetcher);
    perf.Aggregate(TestOneBatch(net_, step_));
  }
  pthread_join(prefetch_thread_, NULL);
  for(auto* layer:net_->input_layer())
    layer->SetInputData(nullptr);
  prefetcher.Free();
  Solver::phase=Phase::kTrain;
  return perf;
}

void Solver::ReportPerformance(string prefix, Performance perf) {
  LOG(ERROR)<<"Train Step: "<<step_<<" "<<prefix<<" "<<perf.ToString();
  /*
  if(context_->AmIGroupLeader()){
    int toRcv=context_->group_size()-1;
    while(toRcv>0){
      Performance p;
      if(mpi_->TryRead(0, MTYPE_PERFORMANCE, &p)){
        perf.Aggregate(p);
        toRcv--;
      }
    }
    //context_->Send(GlobalContext::kCoordinator, MTYPE_PERFORMANCE, perf);
  }else{
    mpi_->Send(context_->leader(), MTYPE_PERFORMANCE, perf);
 }
 */
}

void Solver::Train(int start_step){
  step_=start_step;
  Prefetcher prefetcher(train_shard_, net_);
  pthread_create(&prefetch_thread_, NULL, &PrefetchData, &prefetcher);
  while (!HasFinished()) {
    Solver::phase=Phase::kTrain;
    pthread_join(prefetch_thread_, NULL);
    for(auto* layer:net_->input_layer())
      layer->SetInputData(nullptr);
    if(!ValidateNow()&&!TestNow())
      pthread_create(&prefetch_thread_, NULL, &PrefetchData, &prefetcher);
    train_perf_.Aggregate(TrainOneBatch(net_, step_));

    if(DisplayNow()){
      ReportPerformance("Train", train_perf_.Avg());
      DebugInfo(net_);
      train_perf_.Reset();
    }
    if(ValidateNow()){
      Performance perf=Test(Phase::kValidation);
      ReportPerformance("Val  ", perf.Avg());
    }
    if(TestNow()){
      Performance perf=Test(Phase::kTest);
      ReportPerformance("Test ", perf.Avg());
    }
    if(CheckpointNow()){
      DoLocalCheckpoint(net_);
    }
    if(ValidateNow()||TestNow())
      pthread_create(&prefetch_thread_, NULL, &PrefetchData, &prefetcher);
    IncStep();
  }
  pthread_join(prefetch_thread_, NULL);
  prefetcher.Free();
}
void Solver::DoLocalCheckpoint(Net* net){
  for(auto* param: net->params()){
    if(param->partition()&&GlobalContext::Get()->num_groups()==1){
      DAryProto data;
      param->data().ToProto(&data, true);
      DAryProto grad;
      param->history().ToProto(&grad, true);
      char fname[256];
      sprintf(fname, "%s/local_cp/param_%d_%d.dat", context_->shard_folder().c_str(),
          param->id(), step_);
      WriteProtoToBinaryFile(data, fname);
      sprintf(fname, "%s/local_cp/param_%d_%d.hst", context_->shard_folder().c_str(),
          param->id(), step_);
      WriteProtoToBinaryFile(grad, fname);
    }
  }
}
void Solver::DebugInfo(Net* net){
  char display[4096];
  auto layers=net->layers();
  LOG(INFO)<<"Train Step: "<<step_;
  for(auto* layer: layers){
    sprintf(display, "Forward layer  %10s data norm1 %13.9f",
        layer->name().c_str(), layer->data().Norm1());
    LOG(INFO)<<string(display);
  }
  for (auto layer = layers.rbegin(); layer != layers.rend(); layer++){
    sprintf(display, "Backward layer %10s grad norm1 %13.9f",
        (*layer)->name().c_str(), (*layer)->grad().Norm1());
    LOG(INFO)<<string(display);
  }
  for(auto* layer: layers){
    for(auto* param: layer->GetParams()){
      sprintf(display, "Layer %10s, param id %2d, name %10s, value norm1 %13.9f , grad norm1 %13.9f",
          layer->name().c_str(), param->id(),
          param->name().c_str(), param->data().Norm1(), param->grad().Norm1());
      LOG(INFO)<<string(display);
    }
  }
}



Performance Solver::TrainOneBatch(Net *net, int step){
  auto layers=net->layers();
  if(step==0){
    for(auto* param: net->params()){
      if(!param->partition()||GlobalContext::Get()->num_groups()>1)
        delegate_->AsyncGet(param,step);
    }
  }
  for(auto* layer: layers){
    for(auto* param: layer->GetParams()){
      if(!param->partition()||GlobalContext::Get()->num_groups()>1)
        delegate_->AsyncCollect(param, step);
    }
    if(layer->PreSyncF())
      MPI_Barrier(context_->mpicomm());
    layer->ComputeFeature();
    if(layer->PostSyncF())
      MPI_Barrier(context_->mpicomm());
  }
  Performance perf=net->output_layer(0)->CalcPerf(true, false);
  for (auto layer = layers.rbegin(); layer != layers.rend(); layer++){
    if((*layer)->PreSyncG())
      MPI_Barrier(context_->mpicomm());
    (*layer)->ComputeGradient();
    for(auto* param: (*layer)->GetParams()){
      if(!param->partition()||GlobalContext::Get()->num_groups()>1){
        delegate_->Update(param, step);
        delegate_->AsyncGet(param,step+1);
      }else{
        //LocalUpdate(param, step);
      }
    }
    if((*layer)->PostSyncG())
      MPI_Barrier(context_->mpicomm());
  }
  //DebugInfo(net);
  return perf;
}

Performance Solver::TestOneBatch(Net *net, int step){
  for(auto* layer: net->layers()){
    if(layer->PreSyncF())
      MPI_Barrier(context_->mpicomm());
    layer->ComputeFeature();
    if(layer->PostSyncF())
      MPI_Barrier(context_->mpicomm());
  }
  return net->output_layer(0)->CalcPerf(true, true);
}

void Solver::TimeTableServer(int runs){

}
void Solver::TimeOneBatch(int runs) {
  phase=Phase::kTrain;
  Prefetcher prefetcher(train_shard_, net_);
  PrefetchData(&prefetcher);
  for(auto* layer:net_->input_layer())
    layer->SetInputData(nullptr);

  auto layers=net_->layers();
  int nlayers=layers.size();
  double* forward=new double[nlayers+1];;
  double* backward=new double[nlayers+1];;
  double* refresh=new double[nlayers+1];;
  double* sync=new double[nlayers+1];;
  memset(forward, 0, sizeof(double)*(1+nlayers));
  memset(backward, 0, sizeof(double)*(1+nlayers));
  memset(refresh, 0, sizeof(double)*(1+nlayers));
  memset(sync, 0, sizeof(double)*(1+nlayers));

  LOG(ERROR)<<"Time One Batch...";;
  double sync_start, refresh_start, comp_start;
  //delegate_->StartCollectThread();
  for(auto* param: net_->params()){
    if(!param->partition()||GlobalContext::Get()->num_groups()>1)
      delegate_->AsyncGet(param,step_);
  }

  MPI_Barrier(context_->mpicomm());
  double start=Now();
  for(int k=0;k<runs;k++){
    int layerid=0;
    bool do_collect=true;
    for(auto* layer: layers){
      refresh_start=Now();
      if(layer->name()=="rxxx")
        do_collect=false;
      if(do_collect){
        for(auto* param: layer->GetParams()){
          if(!param->partition()||GlobalContext::Get()->num_groups()>1)
            delegate_->AsyncCollect(param, step_);
        }
      }
      sync_start=Now();
      refresh[layerid]+=sync_start-refresh_start;
      if(layer->PreSyncF())
        MPI_Barrier(context_->mpicomm());
      comp_start=Now();
      sync[layerid]+=comp_start-sync_start;
      layer->ComputeFeature();
      sync_start=Now();
      forward[layerid]+=sync_start-comp_start;
      if(layer->PostSyncF())
        MPI_Barrier(context_->mpicomm());
      sync[layerid]+=Now()-sync_start;
      layerid++;
    }

    for (auto layer = layers.rbegin(); layer != layers.rend(); layer++){
      layerid--;
      sync_start=Now();
      if((*layer)->PreSyncG())
        MPI_Barrier(context_->mpicomm());
      comp_start=Now();
      sync[layerid]+=comp_start-sync_start;
      (*layer)->ComputeGradient();
      refresh_start=Now();
      backward[layerid]+=refresh_start-comp_start;

      if(do_collect){
        for(auto* param: (*layer)->GetParams()){
          if(!param->partition()||GlobalContext::Get()->num_groups()>1){
            delegate_->Update(param, step_);
            delegate_->AsyncGet(param, step_+1);
          }
        }
      }
      if((*layer)->name()=="rxxx")
        do_collect=true;
      sync_start=Now();
      refresh[layerid]+=sync_start-refresh_start;
      if((*layer)->PostSyncG())
        MPI_Barrier(context_->mpicomm());
      sync[layerid]+=Now()-sync_start;
    }
    IncStep();
    LOG(ERROR)<<"one iter";
  }
  //delegate_->StopCollectThread();
  double total=Now()-start;
  MPI_Barrier(context_->mpicomm());
  LOG(ERROR)<<"Finish";
  int K=1024;
  char buf[10*K];
  sprintf(buf, "\n");
  for(int i=0;i<nlayers;i++){
    sprintf(buf+strlen(buf), "Layer %10s forward %6.2f backward %6.2f sync %6.2f refresh %6.2f\n",
        layers[i]->name().c_str(),forward[i]/runs, backward[i]/runs, sync[i]/runs, refresh[i]/runs);
    forward[nlayers]+=forward[i];
    backward[nlayers]+=backward[i];
    sync[nlayers]+=sync[i];
    refresh[nlayers]+=refresh[i];
  }
  double armcitime=GAry::comm_time;
  sprintf(buf+strlen(buf), "Total\t%6.2f\tforward\t%6.2f\tbackward\t%6.2f\tcomp\t%6.2f\tsync\t%6.2f\trefresh\t%6.2f\tarmci\t%6.2f\n",
      total/runs,forward[nlayers]/runs, backward[nlayers]/runs, (forward[nlayers]+backward[nlayers]-armcitime)/runs, sync[nlayers]/runs,
      refresh[nlayers]/runs, armcitime/runs);
  LOG(ERROR)<<string(buf);
  delete forward;
  delete backward;
  delete sync;
  delete refresh;
  DoLocalCheckpoint(net_);
  //DebugInfo(net_);
}

//Performance Solver::Test(Net *net) { }
bool Solver::HasFinished(){
  if (step_ >= train_steps_)
    return true;
  else
    return false;
}

}  // namespace lapis
