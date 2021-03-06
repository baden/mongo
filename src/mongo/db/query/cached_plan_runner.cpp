/**
 *    Copyright (C) 2013 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/db/query/cached_plan_runner.h"

#include "mongo/db/diskloc.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/exec/working_set.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/explain_plan.h"
#include "mongo/db/query/plan_cache.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/type_explain.h"

namespace mongo {

    CachedPlanRunner::CachedPlanRunner(CanonicalQuery* canonicalQuery,
                                       CachedSolution* cached,
                                       PlanStage* root,
                                       WorkingSet* ws)
        : _canonicalQuery(canonicalQuery),
          _cachedQuery(cached),
          _exec(new PlanExecutor(ws, root)),
          _updatedCache(false) {
    }

    CachedPlanRunner::~CachedPlanRunner() {
    }

    Runner::RunnerState CachedPlanRunner::getNext(BSONObj* objOut, DiskLoc* dlOut) {
        Runner::RunnerState state = _exec->getNext(objOut, dlOut);
        if (Runner::RUNNER_EOF == state && !_updatedCache) {
            updateCache();
        }
        return state;
    }

    bool CachedPlanRunner::isEOF() {
        return _exec->isEOF();
    }

    void CachedPlanRunner::saveState() {
        _exec->saveState();
    }

    bool CachedPlanRunner::restoreState() {
        return _exec->restoreState();
    }

    void CachedPlanRunner::invalidate(const DiskLoc& dl) {
        _exec->invalidate(dl);
    }

    void CachedPlanRunner::setYieldPolicy(Runner::YieldPolicy policy) {
        _exec->setYieldPolicy(policy);
    }

    const std::string& CachedPlanRunner::ns() {
        return _canonicalQuery->getParsed().ns();
    }

    void CachedPlanRunner::kill() {
        _exec->kill();
    }

    Status CachedPlanRunner::getExplainPlan(TypeExplain** explain) const {
        dassert(_exec.get());

        scoped_ptr<PlanStageStats> stats(_exec->getStats());
        if (NULL == stats.get()) {
            return Status(ErrorCodes::InternalError, "no stats available to explain plan");
        }

        Status status = explainPlan(*stats, explain, true /* full details */);
        if (!status.isOK()) {
            return status;
        }

        // Fill in explain fields that are accounted by on the runner level.
        TypeExplain* chosenPlan = NULL;
        explainPlan(*stats, &chosenPlan, false /* no full details */);
        if (chosenPlan) {
            (*explain)->addToAllPlans(chosenPlan);
        }
        (*explain)->setNScannedObjectsAllPlans((*explain)->getNScannedObjects());
        (*explain)->setNScannedAllPlans((*explain)->getNScanned());

        return Status::OK();
    }

    void CachedPlanRunner::updateCache() {
        _updatedCache = true;

        // We're done.  Update the cache.
        PlanCache* cache = PlanCache::get(_canonicalQuery->ns());

        // TODO: Is this an error?
        if (NULL == cache) { return; }

        // TODO: How do we decide this?
        bool shouldRemovePlan = false;

        if (shouldRemovePlan) {
            if (!cache->remove(*_canonicalQuery, *_cachedQuery->solution)) {
                warning() << "Cached plan runner couldn't remove plan from cache.  Maybe"
                    " somebody else did already?";
                return;
            }
        }

        // We're done running.  Update cache.
        auto_ptr<CachedSolutionFeedback> feedback(new CachedSolutionFeedback());
        feedback->stats = _exec->getStats();
        cache->feedback(*_canonicalQuery, *_cachedQuery->solution, feedback.release());
    }

} // namespace mongo
