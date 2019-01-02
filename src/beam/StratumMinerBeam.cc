/*
 The MIT License (MIT)

 Copyright (c) [2016] [BTC.COM]

 Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated documentation files (the "Software"), to deal
 in the Software without restriction, including without limitation the rights
 to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 copies of the Software, and to permit persons to whom the Software is
 furnished to do so, subject to the following conditions:

 The above copyright notice and this permission notice shall be included in
 all copies or substantial portions of the Software.

 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 THE SOFTWARE.
 */
#include "StratumMinerBeam.h"

#include "StratumSessionBeam.h"
#include "StratumServerBeam.h"
#include "DiffController.h"

#include "CommonBeam.h"

///////////////////////////////// StratumSessionBeam ////////////////////////////////
StratumMinerBeam::StratumMinerBeam(StratumSessionBeam &session,
                                 const DiffController &diffController,
                                 const std::string &clientAgent,
                                 const std::string &workerName,
                                 int64_t workerId)
    : StratumMinerBase(session, diffController, clientAgent, workerName, workerId) {
}

void StratumMinerBeam::handleRequest(const std::string &idStr,
                                    const std::string &method,
                                    const JsonNode &jparams,
                                    const JsonNode &jroot) {
  if (method == "solution") {
    handleRequest_Submit(idStr, jroot);
  }
}

void StratumMinerBeam::handleRequest_Submit(const string &idStr, const JsonNode &jroot) {
  // const type cannot access string indexed object member
  JsonNode &jsonRoot = const_cast<JsonNode &>(jroot);

  auto &session = getSession();
  if (session.getState() != StratumSession::AUTHENTICATED) {
    session.responseError(idStr, StratumStatus::UNAUTHORIZED);
    return;
  }

  if (
    jsonRoot["id"].type() != Utilities::JS::type::Str ||
    jsonRoot["nonce"].type() != Utilities::JS::type::Str ||
    jsonRoot["output"].type() != Utilities::JS::type::Str
  ) {
    session.responseError(idStr, StratumStatus::ILLEGAL_PARARMS);
    return;
  }

  uint32_t jobId = jsonRoot["id"].uint32_hex();
  uint64_t nonce = jsonRoot["nonce"].uint64_hex();
  string output = jsonRoot["output"].str();

  auto localJob = session.findLocalJob(jobId);
  // can't find local job
  if (localJob == nullptr) {
    session.responseFalse(idStr, StratumStatus::JOB_NOT_FOUND);
    return;
  }

  auto &server = session.getServer();
  auto &worker = session.getWorker();
  auto sessionId = session.getSessionId();
  auto clientIp = session.getClientIp();
  
  shared_ptr<StratumJobEx> exjob = server.GetJobRepository(localJob->chainId_)
    ->getStratumJobEx(localJob->jobId_);
  // can't find stratum job
  if (exjob.get() == nullptr) {
    session.responseFalse(idStr, StratumStatus::JOB_NOT_FOUND);
    return;
  }
  StratumJobBeam *sjob = dynamic_cast<StratumJobBeam *>(exjob->sjob_);

  // Used to prevent duplicate shares. (sHeader has a prefix "0x")
  uint64_t inputPrefix = stoull(sjob->input_.substr(0, 16), nullptr, 16);

  auto iter = jobDiffs_.find(localJob);
  if (iter == jobDiffs_.end()) {
    LOG(ERROR) << "can't find session's diff, worker: " << worker.fullName_;
    return;
  }
  auto &jobDiff = iter->second;

  ShareBeam share;
  share.set_version(ShareBeam::CURRENT_VERSION);
  share.set_inputprefix(inputPrefix);
  share.set_workerhashid(workerId_);
  share.set_userid(worker.userId_);
  share.set_sharediff(jobDiff.currentJobDiff_);
  share.set_blockbits(sjob->blockBits_);
  share.set_timestamp((uint64_t) time(nullptr));
  share.set_status(StratumStatus::REJECT_NO_REASON);
  share.set_height(sjob->height_);
  share.set_nonce(nonce);
  share.set_sessionid(sessionId); // TODO: fix it, set as real session id.
  IpAddress ip;
  ip.fromIpv4Int(session.getClientIp());
  share.set_ip(ip.toString());

  LocalShare localShare(nonce, 0, 0);
  // can't add local share
  if (!localJob->addLocalShare(localShare)) {
    session.responseFalse(idStr, StratumStatus::DUPLICATE_SHARE);
    // add invalid share to counter
    invalidSharesCounter_.insert((int64_t) time(nullptr), 1);
    return;
  }

  share.set_status(server.checkShareAndUpdateDiff(
    localJob->chainId_,
    share,
    exjob,
    output,
    jobDiff.jobDiffs_,
    worker.fullName_
  ));

  if (StratumStatus::isAccepted(share.status())) {
    DLOG(INFO) << "share reached the diff: " << share.sharediff();
  } else {
    DLOG(INFO) << "share not reached the diff: " << share.sharediff();
  }

  // we send share to kafka by default, but if there are lots of invalid
  // shares in a short time, we just drop them.
  if (handleShare(idStr, share.status(), share.sharediff())) {
    if (StratumStatus::isSolved(share.status())) {
      server.sendSolvedShare2Kafka(
        localJob->chainId_,
        share,
        sjob->input_,
        output,
        worker
      );
    }
  } else {
    // check if there is invalid share spamming
    int64_t invalidSharesNum = invalidSharesCounter_.sum(time(nullptr), INVALID_SHARE_SLIDING_WINDOWS_SIZE);
    // too much invalid shares, don't send them to kafka
    if (invalidSharesNum >= INVALID_SHARE_SLIDING_WINDOWS_MAX_LIMIT) {
      LOG(WARNING) << "invalid share spamming, diff: "
                   << share.sharediff() << ", uid: " << worker.userId_
                   << ", uname: \"" << worker.userName_ << "\", ip: " << clientIp
                   << "checkshare result: " << share.status();
      return;
    }
  }

  DLOG(INFO) << share.toString();

  std::string message;
  uint32_t size = 0;
  if (!share.SerializeToArrayWithVersion(message, size)) {
    LOG(ERROR) << "share SerializeToBuffer failed!"<< share.toString();
    return;
  }

  server.sendShare2Kafka(localJob->chainId_, message.data(), size);
}