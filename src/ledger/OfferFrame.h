#pragma once

// Copyright 2014 Fonero Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/EntryFrame.h"
#include "ledger/TrustFrame.h"
#include <functional>
#include <unordered_map>

namespace soci
{
class session;
}

namespace fonero
{
class LedgerRange;
class ManageOfferOpFrame;
class StatementContext;

int64_t getSellingLiabilities(OfferEntry const& oe);
int64_t getBuyingLiabilities(OfferEntry const& oe);

class OfferFrame : public EntryFrame
{
    static void
    loadOffers(StatementContext& prep,
               std::function<void(LedgerEntry const&)> offerProcessor);

    double computePrice() const;

    OfferEntry& mOffer;

    void storeUpdateHelper(LedgerDelta& delta, Database& db, bool insert);

  public:
    typedef std::shared_ptr<OfferFrame> pointer;

    enum OfferFlags
    {
        PASSIVE_FLAG = 1
    };

    OfferFrame();
    OfferFrame(LedgerEntry const& from);
    OfferFrame(OfferFrame const& from);

    OfferFrame& operator=(OfferFrame const& other);

    EntryFrame::pointer
    copy() const override
    {
        return std::make_shared<OfferFrame>(*this);
    }

    Price const& getPrice() const;
    int64_t getAmount() const;
    AccountID const& getSellerID() const;
    Asset const& getBuying() const;
    Asset const& getSelling() const;
    uint64 getOfferID() const;
    uint32 getFlags() const;

    int64_t getSellingLiabilities() const;
    int64_t getBuyingLiabilities() const;

    OfferEntry const&
    getOffer() const
    {
        return mOffer;
    }
    OfferEntry&
    getOffer()
    {
        return mOffer;
    }

    // Instance-based overrides of EntryFrame.
    void storeDelete(LedgerDelta& delta, Database& db) const override;
    void storeChange(LedgerDelta& delta, Database& db) override;
    void storeAdd(LedgerDelta& delta, Database& db) override;

    // Static helpers that don't assume an instance.
    static void storeDelete(LedgerDelta& delta, Database& db,
                            LedgerKey const& key);
    static bool exists(Database& db, LedgerKey const& key);
    static uint64_t countObjects(soci::session& sess);
    static uint64_t countObjects(soci::session& sess,
                                 LedgerRange const& ledgers);
    static void deleteOffersModifiedOnOrAfterLedger(Database& db,
                                                    uint32_t oldestLedger);

    // database utilities
    static pointer loadOffer(AccountID const& accountID, uint64_t offerID,
                             Database& db, LedgerDelta* delta = nullptr);

    static void loadBestOffers(size_t numOffers, size_t offset,
                               Asset const& pays, Asset const& gets,
                               std::vector<OfferFrame::pointer>& retOffers,
                               Database& db);

    // load all offers from the database (very slow)
    static std::unordered_map<AccountID, std::vector<OfferFrame::pointer>>
    loadAllOffers(Database& db);

    static std::vector<OfferFrame::pointer>
    loadOffersByAccountAndAsset(AccountID const& accountID, Asset const& asset,
                                Database& db);

    static void dropAll(Database& db);

    void releaseLiabilities(AccountFrame::pointer const& account,
                            TrustFrame::pointer const& buyingTrust,
                            TrustFrame::pointer const& sellingTrust,
                            LedgerDelta& delta, Database& db,
                            LedgerManager& ledgerManager);
    void acquireLiabilities(AccountFrame::pointer const& account,
                            TrustFrame::pointer const& buyingTrust,
                            TrustFrame::pointer const& sellingTrust,
                            LedgerDelta& delta, Database& db,
                            LedgerManager& ledgerManager);

  private:
    void acquireOrReleaseLiabilities(bool isAcquire,
                                     AccountFrame::pointer const& account,
                                     TrustFrame::pointer const& buyingTrust,
                                     TrustFrame::pointer const& sellingTrust,
                                     LedgerDelta& delta, Database& db,
                                     LedgerManager& ledgerManager);

    static const char* kSQLCreateStatement1;
    static const char* kSQLCreateStatement2;
    static const char* kSQLCreateStatement3;
    static const char* kSQLCreateStatement4;
};
}
