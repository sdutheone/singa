// Copyright © 2014 Wei Wang. All Rights Reserved.
// 2014-07-15 18:40

#ifndef INCLUDE_MODEL_INNER_PRODUCT_EDGE_H_
#define INCLUDE_MODEL_INNER_PRODUCT_EDGE_H_

#include <string>
#include <map>
#include "model/edge.h"
#include "model/param.h"
#include "model/blob.h"

namespace lapis {
class InnerProductEdge : public Edge {
 public:
  /**
   * Identifier of this edge, used to set the type field of EdgeProto
   */
  static const std::string kInnerProductEdge;
  virtual void Init(const EdgeProto &edge_proto,
                    const std::map<std::string, Edge *> &edge_map);
  virtual void ToProto(EdgeProto *edge_proto);
  /**
   * Read src Blob multiply with weight Param, plus bias Param and set the
   * result to the dest Blob
   * @param src feature/output Blob from the start layer of the edge.
   * @param dest activation/input Blob of the end layer of the edge.
   */
  virtual void Forward(const Blob *src, Blob *dest, bool overwrite);
  /**
   * Compute gradient w.r.t weight, bias, and feature of the start layer of
   * this edge.
   * @param src gradient w.r.t the output generated by this edge in ::Forward()
   * @param dest gradient Blob corresponding to the src Blob in ::Forward()
   */
  virtual void Backward(const Blob *src_grad, const Blob *dest_fea,
                        Blob *dest_grad, bool overwrite);

  virtual void ComputeParamUpdates(const Trainer *trainer);

 private:
  Param weight_, bias_;
};

}  // namespace lapis

#endif  // INCLUDE_MODEL_INNER_PRODUCT_EDGE_H_
