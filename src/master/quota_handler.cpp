// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License

#include "master/master.hpp"

#include <memory>
#include <vector>

#include <mesos/resources.hpp>

#include <mesos/authorizer/authorizer.hpp>

#include <mesos/quota/quota.hpp>

#include <mesos/resource_quantities.hpp>

#include <process/collect.hpp>
#include <process/defer.hpp>
#include <process/future.hpp>
#include <process/http.hpp>
#include <process/owned.hpp>

#include <stout/json.hpp>
#include <stout/protobuf.hpp>
#include <stout/stringify.hpp>
#include <stout/strings.hpp>
#include <stout/utils.hpp>

#include "common/authorization.hpp"

#include "logging/logging.hpp"

#include "master/quota.hpp"
#include "master/registrar.hpp"

namespace http = process::http;

using google::protobuf::RepeatedPtrField;

using http::Accepted;
using http::BadRequest;
using http::Conflict;
using http::Forbidden;
using http::NotImplemented;
using http::OK;

using mesos::authorization::createSubject;

using mesos::quota::QuotaConfig;
using mesos::quota::QuotaInfo;
using mesos::quota::QuotaRequest;
using mesos::quota::QuotaStatus;

using process::Future;
using process::Owned;

using process::http::authentication::Principal;

using std::string;
using std::unique_ptr;
using std::vector;

namespace mesos {
namespace internal {
namespace master {

// Represents the tree of roles that have quota. The quota guarantees of a child
// node is "contained" in the guarantees of its parent node. This has two
// implications:
//
//   (1) The quota guarantees of a parent must be greater than or equal to the
//       sum of the quota guarantees of its children.
//
//   (2) When computing the total resources guaranteed by quota, we
//       don't want to double-count resource guarantees between a
//       parent role and its children.
//
// TODO(mzhu): The above check is only about guarantees. We should extend
// the check to also cover limits: a role's limit is less than its
// parent's limit.
class QuotaTree
{
public:
  QuotaTree(const hashmap<string, Quota>& quotas)
    : root(new Node(""))
  {
    foreachpair (const string& role, const Quota& quota, quotas) {
      update(role, quota);
    }
  }

  void update(const string& role, const Quota& quota)
  {
    // Create the path from root->leaf in the tree. Any missing nodes
    // are created implicitly.
    vector<string> components = strings::tokenize(role, "/");
    CHECK(!components.empty());

    Node* current = root.get();
    foreach (const string& component, components) {
      if (!current->children.contains(component)) {
        current->children[component] = unique_ptr<Node>(new Node(component));
      }

      current = current->children.at(component).get();
    }

    current->quota = quota;
  }

  // Check whether the tree satisfies:
  //
  //   parent guarantees >= sum(children guarantees)
  //
  // TODO(mzhu): Add limit check.
  Option<Error> validate() const
  {
    // Don't check the root node because it does not have quota set.
    foreachvalue (const unique_ptr<Node>& child, root->children) {
      Option<Error> error = child->validate();
      if (error.isSome()) {
        return error;
      }
    }

    return None();
  }

  // Returns the total guaranteed resource quantities requested by
  // all quotas in the tree. Since a role's guarantees must be greater
  // than or equal to the sum of the guarantees of its children, we can
  // just sum the guarantees of the top-level roles.
  ResourceQuantities totalGuarantees() const
  {
    ResourceQuantities result;

    // Don't include the root node because it does not have quota set.
    foreachvalue (const unique_ptr<Node>& child, root->children) {
      result += child->quota.guarantees;
    }

    return result;
  }

private:
  struct Node
  {
    Node(const string& _name) : name(_name) {}

    Option<Error> validate() const
    {
      foreachvalue (const unique_ptr<Node>& child, children) {
        Option<Error> error = child->validate();
        if (error.isSome()) {
          return error;
        }
      }

      ResourceQuantities childGuarantees;
      foreachvalue (const unique_ptr<Node>& child, children) {
        childGuarantees += child->quota.guarantees;
      }

      // Check if self guarantees contains sum of children's guarantees.
      if (!quota.guarantees.contains(childGuarantees)) {
        return Error("Invalid quota configuration. Parent role '" + name +
                     "' with guarantees (" + stringify(quota.guarantees) +
                     ") does not contain the sum of its children's" +
                     " guarantees (" + stringify(childGuarantees) + ")");
      }

      return None();
    }

    const string name;
    Quota quota;
    hashmap<string, unique_ptr<Node>> children;
  };

  unique_ptr<Node> root;
};


Option<Error> Master::QuotaHandler::overcommitCheck(
    const vector<Resources>& agents,
    const hashmap<string, Quota>& quotas,
    const QuotaInfo& request)
{
  ResourceQuantities totalGuarantees = [&]() {
    QuotaTree quotaTree({});

    foreachpair (const string& role, const Quota& quota, quotas) {
        quotaTree.update(role, quota);
    }

    quotaTree.update(request.role(), Quota{request});

    // Hard CHECK since this is already validated earlier
    // during request validation.
    CHECK_NONE(quotaTree.validate());

    return quotaTree.totalGuarantees();
  }();

  // Determine whether quota overcommits the cluster.
  ResourceQuantities capacity;

  foreach (const Resources& agent, agents) {
    capacity += ResourceQuantities::fromScalarResources(
        agent.nonRevocable().scalars());
  }

  if (!capacity.contains(totalGuarantees)) {
    // TODO(bmahler): Specialize this message based on whether
    // this request leads to the overcommit vs the quota was
    // already overcommitted.
    return Error(
        "Total quota guarantees '" + stringify(totalGuarantees) + "'"
        " exceed cluster capacity '" + stringify(capacity) + "'");
  }

  return None();
}


void Master::QuotaHandler::rescindOffers(const QuotaInfo& request) const
{
  const string& role = request.role();

  // This should have been validated earlier.
  CHECK(master->isWhitelistedRole(role));

  int frameworksInRole = 0;
  if (master->roles.contains(role)) {
    Role* roleState = master->roles.at(role);
    foreachvalue (const Framework* framework, roleState->frameworks) {
      if (framework->active()) {
        ++frameworksInRole;
      }
    }
  }

  // The resources recovered by rescinding outstanding offers.
  Resources rescinded;

  int visitedAgents = 0;

  // Because resources are allocated in the allocator, there can be a race
  // between rescinding and allocating. This race makes it hard to determine
  // the exact amount of offers that should be rescinded in the master.
  //
  // We pessimistically assume that what seems like "available" resources
  // in the allocator will be gone. We greedily rescind all offers from an
  // agent at once until we have rescinded "enough" offers. Offers containing
  // resources irrelevant to the quota request may be rescinded, as we
  // rescind all offers on an agent. This is done to maintain the
  // coarse-grained nature of agent offers, and helps reduce fragmentation of
  // offers.
  //
  // Consider a quota request for role `role` for `requested` resources.
  // There are `numFiR` frameworks in `role`. Let `rescinded` be the total
  // number of rescinded resources and `numVA` be the number of visited
  // agents, from which at least one offer has been rescinded. Then the
  // algorithm can be summarized as follows:
  //
  //   while (there are agents with outstanding offers) do:
  //     if ((`rescinded` contains `requested`) && (`numVA` >= `numFiR`) break;
  //     fetch an agent `a` with outstanding offers;
  //     rescind all outstanding offers from `a`;
  //     update `rescinded`, inc(numVA);
  //   end.
  foreachvalue (const Slave* slave, master->slaves.registered) {
    // If we have rescinded offers with at least as many resources as the
    // quota request resources, then we are done.
    if (rescinded.contains(request.guarantee()) &&
        (visitedAgents >= frameworksInRole)) {
      break;
    }

    // As in the capacity heuristic, we do not consider disconnected or
    // inactive agents, because they do not participate in resource
    // allocation.
    if (!slave->connected || !slave->active) {
      continue;
    }

    // TODO(alexr): Consider only rescinding from agents that have at least
    // one resource relevant to the quota request.

    // Rescind all outstanding offers from the given agent.
    bool agentVisited = false;
    foreach (Offer* offer, utils::copy(slave->offers)) {
      master->allocator->recoverResources(
          offer->framework_id(), offer->slave_id(), offer->resources(), None());

      auto unallocated = [](const Resources& resources) {
        Resources result = resources;
        result.unallocate();
        return result;
      };

      rescinded += unallocated(offer->resources());
      master->removeOffer(offer, true);
      agentVisited = true;
    }

    if (agentVisited) {
      ++visitedAgents;
    }
  }
}


Future<http::Response> Master::QuotaHandler::status(
    const mesos::master::Call& call,
    const Option<Principal>& principal,
    ContentType contentType) const
{
  CHECK_EQ(mesos::master::Call::GET_QUOTA, call.type());

  return _status(principal)
    .then([contentType](const QuotaStatus& status) -> Future<http::Response> {
      mesos::master::Response response;
      response.set_type(mesos::master::Response::GET_QUOTA);
      response.mutable_get_quota()->mutable_status()->CopyFrom(status);

      return OK(serialize(contentType, evolve(response)),
                stringify(contentType));
    });
}


Future<http::Response> Master::QuotaHandler::status(
    const http::Request& request,
    const Option<Principal>& principal) const
{
  VLOG(1) << "Handling quota status request";

  // Check that the request type is GET which is guaranteed by the master.
  CHECK_EQ("GET", request.method);

  return _status(principal)
    .then([request](const QuotaStatus& status) -> Future<http::Response> {
      return OK(JSON::protobuf(status), request.url.query.get("jsonp"));
    });
}


Future<QuotaStatus> Master::QuotaHandler::_status(
    const Option<Principal>& principal) const
{
  // Quotas can be updated during preparation of the response.
  // Copy current view of the collection to avoid conflicts.
  vector<QuotaInfo> quotaInfos;
  quotaInfos.reserve(master->quotas.size());

  foreachpair (const string& role, const Quota& quota, master->quotas) {
    quotaInfos.push_back([&role, &quota]() {
      // Construct the legacy `QuotaInfo`.
      //
      // This is needed for backwards compatibility reasons.
      // Authorizable action `GET_QUOTA` expects an object
      // with `QuotaInfo` set.
      //
      // TODO(mzhu): we plan to deprecate the use of `QuotaInfo`
      // in the `GET_QUOTA` action. And instead, just set the value
      // field in the object using the role. This legacy construction
      // will be removed.
      QuotaInfo info;
      info.set_role(role);
      foreach (auto& quantity, quota.guarantees) {
        Resource resource;
        resource.set_type(Value::SCALAR);
        *resource.mutable_name() = quantity.first;
        *resource.mutable_scalar() = quantity.second;

        *info.add_guarantee() = std::move(resource);
      }
      return info;
    }());
  }

  // Create a list of authorization actions for each role we may return.
  //
  // TODO(alexr): Use an authorization filter here once they are available.
  vector<Future<bool>> authorizedRoles;
  authorizedRoles.reserve(quotaInfos.size());
  foreach (const QuotaInfo& info, quotaInfos) {
    authorizedRoles.push_back(authorizeGetQuota(principal, info));
  }

  return process::collect(authorizedRoles)
    .then(defer(
        master->self(),
        [=](const vector<bool>& authorizedRolesCollected)
            -> Future<QuotaStatus> {
      CHECK(quotaInfos.size() == authorizedRolesCollected.size());

      QuotaStatus status;
      status.mutable_infos()->Reserve(static_cast<int>(quotaInfos.size()));

      // Create an entry (including role and resources) for each quota,
      // except those filtered out based on the authorizer's response.
      //
      // NOTE: This error-prone code will be removed with
      // the introduction of authorization filters.
      auto quotaInfoIt = quotaInfos.begin();
      foreach (const bool& authorized, authorizedRolesCollected) {
        if (authorized) {
          status.add_infos()->CopyFrom(*quotaInfoIt);
        }
        ++quotaInfoIt;
      }

      return status;
    }));
}


Future<http::Response> Master::QuotaHandler::update(
    const mesos::master::Call& call, const Option<Principal>& principal) const
{
  CHECK_EQ(mesos::master::Call::UPDATE_QUOTA, call.type());
  CHECK(call.has_update_quota());

  const RepeatedPtrField<QuotaConfig>& configs =
    call.update_quota().quota_configs();

  // Validate `QuotaConfig`.
  foreach (const auto& config, configs) {
    // Check that the role is on the role whitelist, if it exists.
    if (!master->isWhitelistedRole(config.role())) {
      return BadRequest(
          "Invalid QuotaConfig: '" + config.role() + "'"
          " is not on the roles whitelist");
    }

    // Setting quota on a nested role is temporarily disabled.
    //
    // TODO(mzhu): Remove this check when MESOS-7402 is fixed.
    bool nestedRole = strings::contains(config.role(), "/");
    if (nestedRole) {
      return BadRequest(
          "Updating quota on nested role '" + config.role() +
          "' is not supported yet");
    }

    Option<Error> error = quota::validate(config);

    if (error.isSome()) {
      return BadRequest(
          "Invalid QuotaConfig: " + error->message);
    }
  }

  // TODO(mzhu): Validate a role's limit is below its current consumption
  // (otherwise a `force` flag is needed).
  //
  // TODO(mzhu): Pull out these validation in a function that can be shared
  // between this and the old handlers.

  // Validate hierarchical quota.

  // TODO(mzhu): Keep an up-to-date `QuotaTree` in memory.
  QuotaTree quotaTree{{}};

  foreachpair (const string& role, const Quota& quota, master->quotas) {
    quotaTree.update(role, quota);
  }

  foreach (const auto& config, configs) {
    quotaTree.update(config.role(), Quota{config});
  }

  Option<Error> error = quotaTree.validate();
  if (error.isSome()) {
    return BadRequest("Invalid QuotaConfig: " + error->message);
  }

  // Overcommitment check.

  // Check for quota overcommit. We include resources from all
  // registered agents, even if they are disconnected.
  //
  // Disconnection tends to be a transient state (e.g. agent
  // might be getting restarted as part of an upgrade, there
  // might be a transient networking issue, etc), so excluding
  // disconnected agents could produce an unstable capacity
  // calculation.
  //
  // TODO(bmahler): In the same vein, include agents that
  // are recovered from the registry but not yet registered.
  // Because we currently exclude them, the calculated capacity
  // is 0 immediately after a failover and slowly works its way
  // up to the pre-failover capacity as the agents re-register.
  ResourceQuantities clusterCapacity;
  foreachvalue (const Slave* agent, master->slaves.registered) {
    clusterCapacity += ResourceQuantities::fromScalarResources(
        agent->totalResources.nonRevocable().scalars());
  }

  if (!clusterCapacity.contains(quotaTree.totalGuarantees())) {
    if (call.update_quota().force()) {
      LOG(INFO) << "Using force flag to override quota overcommit check";
    } else {
      return BadRequest("Invalid QuotaConfig: total quota guarantees '" +
              stringify(quotaTree.totalGuarantees()) + "'"
              " exceed cluster capacity '" + stringify(clusterCapacity) + "'"
              " (use 'force' flag to bypass this check)");
    }
  }

  // Create a list of authorization actions
  // for each quota configuration update.
  vector<Future<bool>> authorizedUpdates;
  authorizedUpdates.reserve(configs.size());
  foreach (const QuotaConfig& config, configs) {
    authorizedUpdates.push_back(authorizeUpdateQuotaConfig(principal, config));
  }

  return process::collect(authorizedUpdates)
    .then(defer(
        master->self(),
        [=](const vector<bool>& authorizations) -> Future<http::Response> {
          return std::all_of(
                     authorizations.begin(),
                     authorizations.end(),
                     [](bool authorized) { return authorized; })
                   ? _update(configs)
                   : Forbidden();
        }));
}


Future<http::Response> Master::QuotaHandler::_update(
    const RepeatedPtrField<QuotaConfig>& configs) const
{
  return master->registrar
    ->apply(Owned<RegistryOperation>(new quota::UpdateQuota(configs)))
    .then(defer(master->self(), [=](bool result) -> Future<http::Response> {
      // Currently, quota registry entry mutation never fails.
      CHECK(result);

      foreach (const QuotaConfig& config, configs) {
        master->quotas[config.role()] = Quota(config);
        master->allocator->updateQuota(config.role(), Quota{config});
      }

      // TODO(mzhu): Rescind offers.

      return OK();
    }));
}


Future<http::Response> Master::QuotaHandler::set(
    const mesos::master::Call& call,
    const Option<Principal>& principal) const
{
  CHECK_EQ(mesos::master::Call::SET_QUOTA, call.type());
  CHECK(call.has_set_quota());

  return _set(call.set_quota().quota_request(), principal);
}


Future<http::Response> Master::QuotaHandler::set(
    const http::Request& request,
    const Option<Principal>& principal) const
{
  VLOG(1) << "Setting quota from request: '" << request.body << "'";

  // Check that the request type is POST which is guaranteed by the master.
  CHECK_EQ("POST", request.method);

  // Parse the request body into JSON.
  Try<JSON::Object> jsonRequest = JSON::parse<JSON::Object>(request.body);
  if (jsonRequest.isError()) {
    return BadRequest(
        "Failed to parse set quota request JSON '" + request.body + "': " +
        jsonRequest.error());
  }

  // Convert JSON request to the `QuotaRequest` protobuf.
  Try<QuotaRequest> protoRequest =
    ::protobuf::parse<QuotaRequest>(jsonRequest.get());

  if (protoRequest.isError()) {
    return BadRequest(
        "Failed to validate set quota request JSON '" + request.body + "': " +
        protoRequest.error());
  }

  return _set(protoRequest.get(), principal);
}


Future<http::Response> Master::QuotaHandler::_set(
    const QuotaRequest& quotaRequest,
    const Option<Principal>& principal) const
{
  Try<QuotaInfo> create = quota::createQuotaInfo(quotaRequest);
  if (create.isError()) {
    return BadRequest(
        "Failed to create 'QuotaInfo' from set quota request: " +
        create.error());
  }

  QuotaInfo quotaInfo = create.get();

  // Check that each guarantee/resource is valid.
  Option<Error> validate = Resources::validate(quotaInfo.guarantee());
  if (validate.isSome()) {
    return BadRequest(
        "Failed to validate set quota request:"
        " QuotaInfo with invalid resource: " + validate->message);
  }

  upgradeResources(&quotaInfo);

  // Check that the `QuotaInfo` is a valid quota request.
  {
    Option<Error> error = quota::validation::quotaInfo(quotaInfo);
    if (error.isSome()) {
      return BadRequest(
          "Failed to validate set quota request: " + error->message);
    }
  }

  // Check that the role is on the role whitelist, if it exists.
  if (!master->isWhitelistedRole(quotaInfo.role())) {
    return BadRequest(
        "Failed to validate set quota request: Unknown role '" +
        quotaInfo.role() + "'");
  }

  // Check that we are not updating an existing quota.
  // TODO(joerg84): Update error message once quota update is in place.
  if (master->quotas.contains(quotaInfo.role())) {
    return BadRequest(
        "Failed to validate set quota request: Cannot set quota"
        " for role '" + quotaInfo.role() + "' which already has quota");
  }

  // Validate that adding this quota does not violate the hierarchical
  // relationship between quotas.
  {
     // TODO(mzhu): Keep an update-to-date `QuotaTree` in the memory
     // to avoid construction from scratch every time.
    QuotaTree quotaTree({});

    foreachpair (const string& role, const Quota& quota, master->quotas) {
      quotaTree.update(role, quota);
    }

    quotaTree.update(quotaInfo.role(), Quota{quotaInfo});

    Option<Error> error = quotaTree.validate();
    if (error.isSome()) {
      return BadRequest(
          "Failed to validate set quota request: " + error->message);
    }
  }

  // Setting quota on a nested role is temporarily disabled.
  //
  // TODO(neilc): Remove this check when MESOS-7402 is fixed.
  bool nestedRole = strings::contains(quotaInfo.role(), "/");
  if (nestedRole) {
    return BadRequest("Setting quota on nested role '" +
                      quotaInfo.role() + "' is not supported yet");
  }

  const bool forced = quotaRequest.force();

  if (principal.isSome()) {
    // We assume that `principal->value.isSome()` is true. The master's HTTP
    // handlers enforce this constraint, and V0 authenticators will only return
    // principals of that form.
    CHECK_SOME(principal->value);

    quotaInfo.set_principal(principal->value.get());
  }

  return authorizeUpdateQuota(principal, quotaInfo)
    .then(defer(master->self(), [=](bool authorized) -> Future<http::Response> {
      return !authorized ? Forbidden() : __set(quotaInfo, forced);
    }));
}


Future<http::Response> Master::QuotaHandler::__set(
    const QuotaInfo& quotaInfo,
    bool forced) const
{
  if (forced) {
    VLOG(1) << "Using force flag to override quota capacity heuristic check";
  } else {
    // Check for quota overcommit. We include resources from all
    // registered agents, even if they are disconnected.
    //
    // Disconnection tends to be a transient state (e.g. agent
    // might be getting restarted as part of an upgrade, there
    // might be a transient networking issue, etc), so excluding
    // disconnected agents could produce an unstable capacity
    // calculation.
    //
    // TODO(bmahler): In the same vein, include agents that
    // are recovered from the registry but not yet registered.
    // Because we currently exclude them, the calculated capacity
    // is 0 immediately after a failover and slowly works its way
    // up to the pre-failover capacity as the agents re-register.
    vector<Resources> agents;
    agents.reserve(master->slaves.registered.size());

    foreachvalue (const Slave* agent, master->slaves.registered) {
      agents.push_back(agent->totalResources);
    }

    // Validate whether quota overcommits the cluster capacity.
    Option<Error> error = overcommitCheck(
        agents,
        master->quotas,
        quotaInfo);

    if (error.isSome()) {
      return Conflict(
          "Quota guarantees overcommit the cluster"
          " (use 'force' to bypass this check): " +
          error->message);
    }
  }

  Quota quota = Quota{quotaInfo};

  // Populate master's quota-related local state. We do this before updating
  // the registry in order to make sure that we are not already trying to
  // satisfy a request for this role (since this is a multi-phase event).
  // NOTE: We do not need to remove quota for the role if the registry update
  // fails because in this case the master fails as well.
  master->quotas[quotaInfo.role()] = quota;

  // Construct `RepeatedPtrField<QuotaConfig>` from the legacy `QuotaInfo`
  // for forward compatibility.
  RepeatedPtrField<QuotaConfig> configs = [&quotaInfo]() {
    QuotaConfig config;
    *config.mutable_role() = quotaInfo.role();

    google::protobuf::Map<string, Value::Scalar> quota;
    foreach (const Resource& r, quotaInfo.guarantee()) {
      quota[r.name()] = r.scalar();
    }

    *config.mutable_guarantees() = quota;
    *config.mutable_limits() = std::move(quota);

    RepeatedPtrField<QuotaConfig> configs;
    *configs.Add() = std::move(config);

    return configs;
  }();

  // Update the registry with the new quota and acknowledge the request.
  return master->registrar
    ->apply(Owned<RegistryOperation>(new quota::UpdateQuota(configs)))
    .then(defer(master->self(), [=](bool result) -> Future<http::Response> {
      // See the top comment in "master/quota.hpp" for why this check is here.
      CHECK(result);

      master->allocator->updateQuota(quotaInfo.role(), quota);

      // Rescind outstanding offers to facilitate satisfying the quota request.
      // NOTE: We set quota before we rescind to avoid a race. If we were to
      // rescind first, then recovered resources may get allocated again
      // before our call to `updateQuota` was handled.
      // The consequence of setting quota first is that (in the hierarchical
      // allocator) it will trigger an allocation. This means the rescinded
      // offer resources will only be available to quota once another
      // allocation is invoked.
      // This can be resolved in the future with an explicit allocation call,
      // and this solution is preferred to having the race described earlier.
      rescindOffers(quotaInfo);

      return OK();
    }));
}


Future<http::Response> Master::QuotaHandler::remove(
    const mesos::master::Call& call,
    const Option<Principal>& principal) const
{
  CHECK_EQ(mesos::master::Call::REMOVE_QUOTA, call.type());
  CHECK(call.has_remove_quota());

  return _remove(call.remove_quota().role(), principal);
}


Future<http::Response> Master::QuotaHandler::remove(
    const http::Request& request,
    const Option<Principal>& principal) const
{
  VLOG(1) << "Removing quota for request path: '" << request.url.path << "'";

  // Check that the request type is DELETE which is guaranteed by the master.
  CHECK_EQ("DELETE", request.method);

  // Extract role from url. We expect the request path to have the
  // format "/master/quota/role", where "role" is a role name. The
  // role name itself may contain one or more slashes. Note that
  // `strings::tokenize` returns the remainder of the string when the
  // specified maximum number of tokens is reached.
  vector<string> components = strings::tokenize(request.url.path, "/", 3u);
  if (components.size() < 3u) {
    return BadRequest("Failed to parse remove quota request for path '" +
                      request.url.path + "': expected 3 tokens, found " +
                      stringify(components.size()) + " tokens");
  }

  CHECK_EQ(3u, components.size());
  string role = components.back();

  // Check that the role is on the role whitelist, if it exists.
  if (!master->isWhitelistedRole(role)) {
    return BadRequest(
        "Failed to validate remove quota request for path '" +
        request.url.path + "': Unknown role '" + role + "'");
  }

  // Check that we are removing an existing quota.
  if (!master->quotas.contains(role)) {
    return BadRequest(
        "Failed to remove quota for path '" + request.url.path +
        "': Role '" + role + "' has no quota set");
  }

  hashmap<string, Quota> quotaMap = master->quotas;

  // Validate that removing the quota for `role` does not violate the
  // hierarchical relationship between quotas.
  quotaMap.erase(role);

  QuotaTree quotaTree(quotaMap);

  Option<Error> error = quotaTree.validate();
  if (error.isSome()) {
    return BadRequest(
        "Failed to remove quota for path '" + request.url.path +
        "': " + error->message);
  }

  return _remove(role, principal);
}


Future<http::Response> Master::QuotaHandler::_remove(
    const string& role,
    const Option<Principal>& principal) const
{
  // Construct the legacy `QuotaInfo`. This is needed for backwards
  // compatibility reasons. Authorizable action `UPDATE_QUOTA` which is
  // used for `SET_QUOTA` and `REMOVE_QUOTA` expects an object with
  // `QuotaInfo` set. The new API `UPDATE_QUOTA` uses a different
  // action `UPDATE_QUOTA_WITH_CONFIG`. The old authorizable action
  // and this legacy construction should be removed in Mesos 2.0
  // when we remove the old APIs.
  QuotaInfo info;
  info.set_role(role);
  foreach (const auto& quantity, master->quotas.at(role).guarantees) {
    Resource resource;
    resource.set_type(Value::SCALAR);
    *resource.mutable_name() = quantity.first;
    *resource.mutable_scalar() = quantity.second;

    *info.add_guarantee() = std::move(resource);
  }

  return authorizeUpdateQuota(principal, info)
    .then(defer(master->self(), [=](bool authorized) -> Future<http::Response> {
      return !authorized ? Forbidden() : __remove(role);
    }));
}


Future<http::Response> Master::QuotaHandler::__remove(const string& role) const
{
  // Double check if the quota still exists. It may have been removed
  // by a previous removal already.
  if (!master->quotas.contains(role)) {
    return BadRequest(
        "Failed to remove quota: Role '" + role + "' has no quota set");
  }

  // Remove quota from the quota-related local state. We do this before
  // updating the registry in order to make sure that we are not already
  // trying to remove quota for this role (since this is a multi-phase event).
  // NOTE: We do not need to restore quota for the role if the registry
  // update fails because in this case the master fails as well and quota
  // will be restored automatically during the recovery.
  master->quotas.erase(role);

  // Remove quota is equivalent to configure quota back to the default.
  // We need to wrap it up in `RepeatedPtrField<QuotaConfig>` for
  // foward compatibility.
  RepeatedPtrField<QuotaConfig> configs = [&role]() {
    QuotaConfig config;
    *config.mutable_role() = role;

    RepeatedPtrField<QuotaConfig> configs;
    *configs.Add() = std::move(config);

    return configs;
  }();

  // Update the registry with the removed quota and acknowledge the request.
  return master->registrar
    ->apply(Owned<RegistryOperation>(new quota::UpdateQuota(configs)))
    .then(defer(master->self(), [=](bool result) -> Future<http::Response> {
      // See the top comment in "master/quota.hpp" for why this check is here.
      CHECK(result);

      master->allocator->updateQuota(role, DEFAULT_QUOTA);

      return OK();
    }));
}


Future<bool> Master::QuotaHandler::authorizeGetQuota(
    const Option<Principal>& principal,
    const QuotaInfo& quotaInfo) const
{
  if (master->authorizer.isNone()) {
    return true;
  }

  LOG(INFO) << "Authorizing principal '"
            << (principal.isSome() ? stringify(principal.get()) : "ANY")
            << "' to get quota for role '" << quotaInfo.role() << "'";

  authorization::Request request;
  request.set_action(authorization::GET_QUOTA);

  Option<authorization::Subject> subject = createSubject(principal);
  if (subject.isSome()) {
    request.mutable_subject()->CopyFrom(subject.get());
  }

  // TODO(alexr): The `value` field is set for backwards compatibility
  // reasons until after the deprecation cycle started with 1.2.0 ends.
  request.mutable_object()->mutable_quota_info()->CopyFrom(quotaInfo);
  request.mutable_object()->set_value(quotaInfo.role());

  return master->authorizer.get()->authorized(request);
}


Future<bool> Master::QuotaHandler::authorizeUpdateQuota(
    const Option<Principal>& principal,
    const QuotaInfo& quotaInfo) const
{
  if (master->authorizer.isNone()) {
    return true;
  }

  LOG(INFO) << "Authorizing principal '"
            << (principal.isSome() ? stringify(principal.get()) : "ANY")
            << "' to update quota for role '" << quotaInfo.role() << "'";

  authorization::Request request;
  request.set_action(authorization::UPDATE_QUOTA);

  Option<authorization::Subject> subject = createSubject(principal);
  if (subject.isSome()) {
    request.mutable_subject()->CopyFrom(subject.get());
  }

  request.mutable_object()->mutable_quota_info()->CopyFrom(quotaInfo);

  return master->authorizer.get()->authorized(request);
}


Future<bool> Master::QuotaHandler::authorizeUpdateQuotaConfig(
    const Option<Principal>& principal, const QuotaConfig& quotaConfig) const
{
  if (master->authorizer.isNone()) {
    return true;
  }

  LOG(INFO) << "Authorizing principal '"
            << (principal.isSome() ? stringify(principal.get()) : "ANY")
            << "' to update quota config"
            << " for role '" << quotaConfig.role() << "'";

  authorization::Request request;
  request.set_action(authorization::UPDATE_QUOTA_WITH_CONFIG);

  Option<authorization::Subject> subject = createSubject(principal);
  if (subject.isSome()) {
    *request.mutable_subject() = std::move(*subject);
  }

  *request.mutable_object()->mutable_value() = quotaConfig.role();

  return master->authorizer.get()->authorized(request);
}

} // namespace master {
} // namespace internal {
} // namespace mesos {
