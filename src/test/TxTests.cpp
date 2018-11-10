// Copyright 2014 Fonero Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "test/TxTests.h"
#include "crypto/ByteSlice.h"
#include "crypto/SignerKey.h"
#include "database/Database.h"
#include "invariant/InvariantManager.h"
#include "ledger/DataFrame.h"
#include "ledger/LedgerDelta.h"
#include "main/Application.h"
#include "test/TestExceptions.h"
#include "test/TestUtils.h"
#include "test/test.h"
#include "transactions/AllowTrustOpFrame.h"
#include "transactions/BumpSequenceOpFrame.h"
#include "transactions/ChangeTrustOpFrame.h"
#include "transactions/CreateAccountOpFrame.h"
#include "transactions/InflationOpFrame.h"
#include "transactions/ManageDataOpFrame.h"
#include "transactions/ManageOfferOpFrame.h"
#include "transactions/MergeOpFrame.h"
#include "transactions/PathPaymentOpFrame.h"
#include "transactions/PaymentOpFrame.h"
#include "transactions/SetOptionsOpFrame.h"
#include "transactions/TransactionFrame.h"
#include "util/Logging.h"
#include "util/XDROperators.h"
#include "util/types.h"

#include <lib/catch.hpp>

using namespace fonero;
using namespace fonero::txtest;

typedef std::unique_ptr<Application> appPtr;
namespace fonero
{
namespace txtest
{

ExpectedOpResult::ExpectedOpResult(OperationResultCode code) : code{code}
{
}
ExpectedOpResult::ExpectedOpResult(CreateAccountResultCode createAccountCode)
    : code{opINNER}, type{CREATE_ACCOUNT}, createAccountCode{createAccountCode}
{
}
ExpectedOpResult::ExpectedOpResult(PaymentResultCode paymentCode)
    : code{opINNER}, type{PAYMENT}, paymentCode{paymentCode}
{
}
ExpectedOpResult::ExpectedOpResult(AccountMergeResultCode accountMergeCode)
    : code{opINNER}, type{ACCOUNT_MERGE}, accountMergeCode{accountMergeCode}
{
}
ExpectedOpResult::ExpectedOpResult(SetOptionsResultCode setOptionsResultCode)
    : code{opINNER}
    , type{SET_OPTIONS}
    , setOptionsResultCode{setOptionsResultCode}
{
}

TransactionResult
expectedResult(int64_t fee, size_t opsCount, TransactionResultCode code,
               std::vector<ExpectedOpResult> ops)
{
    auto result = TransactionResult{};
    result.feeCharged = fee;
    result.result.code(code);
    if (code != txSUCCESS && code != txFAILED)
    {
        return result;
    }
    if (ops.empty())
    {
        std::fill_n(std::back_inserter(ops), opsCount, PAYMENT_SUCCESS);
    }

    result.result.results().resize(static_cast<uint32_t>(ops.size()));
    for (size_t i = 0; i < ops.size(); i++)
    {
        auto& r = result.result.results()[i];
        auto& o = ops[i];
        r.code(o.code);
        if (o.code == opINNER)
        {
            r.tr().type(o.type);
            switch (o.type)
            {
            case CREATE_ACCOUNT:
                r.tr().createAccountResult().code(o.createAccountCode);
                break;
            case PAYMENT:
                r.tr().paymentResult().code(o.paymentCode);
                break;
            case ACCOUNT_MERGE:
                r.tr().accountMergeResult().code(o.accountMergeCode);
                break;
            case SET_OPTIONS:
                r.tr().setOptionsResult().code(o.setOptionsResultCode);
                break;
            default:
                break;
            }
        }
    }

    return result;
}

bool
applyCheck(TransactionFramePtr tx, Application& app, bool checkSeqNum)
{
    app.getDatabase().clearPreparedStatementCache();

    LedgerDelta delta(app.getLedgerManager().getCurrentLedgerHeader(),
                      app.getDatabase());

    AccountEntry srcAccountBefore;

    bool check = tx->checkValid(app, 0);
    TransactionResult checkResult = tx->getResult();

    REQUIRE((!check || checkResult.result.code() == txSUCCESS));

    // now, check what happens when simulating what happens during a ledger
    // close and reconcile it with the return value of "apply" with the one from
    // checkValid:
    // * an invalid (as per isValid) tx is still invalid during apply (and the
    // same way)
    // * a valid tx can fail later
    auto code = checkResult.result.code();
    if (code != txNO_ACCOUNT)
    {
        auto acnt = loadAccount(tx->getSourceID(), app, true);
        srcAccountBefore = acnt->getAccount();

        // no account -> can't process the fee
        tx->processFeeSeqNum(delta, app.getLedgerManager());

        // verify that the fee got processed
        auto added = delta.added();
        REQUIRE(added.begin() == added.end());
        auto deleted = delta.deleted();
        REQUIRE(deleted.begin() == deleted.end());
        auto modified = delta.modified();
        REQUIRE(modified.begin() != modified.end());
        int modifiedCount = 0;
        for (auto m : modified)
        {
            modifiedCount++;
            REQUIRE(modifiedCount == 1);
            REQUIRE(m.key.account().accountID == tx->getSourceID());
            auto& prevAccount = m.previous->mEntry.data.account();
            REQUIRE(prevAccount == srcAccountBefore);
            auto curAccount = m.current->mEntry.data.account();
            // the balance should have changed
            REQUIRE(curAccount.balance < prevAccount.balance);
            curAccount.balance = prevAccount.balance;
            if (app.getLedgerManager().getCurrentLedgerVersion() <= 9)
            {
                // v9 and below, we also need to verify that the sequence number
                // also got processed at this time
                REQUIRE(curAccount.seqNum == (prevAccount.seqNum + 1));
                curAccount.seqNum = prevAccount.seqNum;
            }
            REQUIRE(curAccount == prevAccount);
        }
    }

    bool res = false;

    {
        LedgerDelta applyDelta(delta);
        try
        {
            res = tx->apply(applyDelta, app);
        }
        catch (...)
        {
            tx->getResult().result.code(txINTERNAL_ERROR);
        }

        REQUIRE((!res || tx->getResultCode() == txSUCCESS));

        // checks that the failure is the same if pre checks failed
        if (!check)
        {
            if (tx->getResultCode() != txFAILED)
            {
                REQUIRE(checkResult == tx->getResult());
            }
            else
            {
                auto const& txResults = tx->getResult().result.results();
                auto const& checkResults = checkResult.result.results();
                for (auto i = 0u; i < txResults.size(); i++)
                {
                    REQUIRE(checkResults[i] == txResults[i]);
                    if (checkResults[i].code() == opBAD_AUTH)
                    {
                        // results may not match after first opBAD_AUTH
                        break;
                    }
                }
            }
        }

        if (code != txNO_ACCOUNT)
        {
            auto srcAccountAfter =
                txtest::loadAccount(srcAccountBefore.accountID, app, false);
            if (srcAccountAfter)
            {
                bool earlyFailure =
                    (code == txMISSING_OPERATION || code == txTOO_EARLY ||
                     code == txTOO_LATE || code == txINSUFFICIENT_FEE ||
                     code == txBAD_SEQ);
                // verify that the sequence number changed (v10+)
                // do not perform the check if there was a failure before
                // or during the sequence number processing
                if (checkSeqNum &&
                    app.getLedgerManager().getCurrentLedgerVersion() >= 10 &&
                    !earlyFailure)
                {
                    REQUIRE(srcAccountAfter->getSeqNum() ==
                            (srcAccountBefore.seqNum + 1));
                }
                // on failure, no other changes should have been made
                if (!res)
                {
                    auto added = applyDelta.added();
                    REQUIRE(added.begin() == added.end());
                    auto deleted = applyDelta.deleted();
                    REQUIRE(deleted.begin() == deleted.end());
                    auto modified = applyDelta.modified();
                    if (earlyFailure ||
                        app.getLedgerManager().getCurrentLedgerVersion() <= 9)
                    {
                        // no changes during an early failure
                        REQUIRE(modified.begin() == modified.end());
                    }
                    else
                    {
                        REQUIRE(modified.begin() != modified.end());
                        for (auto m : modified)
                        {
                            REQUIRE(m.key.account().accountID ==
                                    srcAccountBefore.accountID);
                            // could check more here if needed
                        }
                    }
                }
            }
        }
        applyDelta.commit();
    }

    // validates db state
    app.getLedgerManager().checkDbState();
    delta.commit();

    return res;
}

void
checkTransaction(TransactionFrame& txFrame, Application& app)
{
    REQUIRE(txFrame.getResult().feeCharged ==
            app.getLedgerManager().getTxFee());
    REQUIRE((txFrame.getResultCode() == txSUCCESS ||
             txFrame.getResultCode() == txFAILED));
}

void
applyTx(TransactionFramePtr const& tx, Application& app, bool checkSeqNum)
{
    applyCheck(tx, app, checkSeqNum);
    throwIf(tx->getResult());
    checkTransaction(*tx, app);
}

void
validateTxResults(TransactionFramePtr const& tx, Application& app,
                  ValidationResult validationResult,
                  TransactionResult const& applyResult)
{
    auto shouldValidateOk = validationResult.code == txSUCCESS;
    REQUIRE(tx->checkValid(app, 0) == shouldValidateOk);
    REQUIRE(tx->getResult().result.code() == validationResult.code);
    REQUIRE(tx->getResult().feeCharged == validationResult.fee);

    // do not try to apply if checkValid returned false
    if (!shouldValidateOk)
    {
        REQUIRE(applyResult == TransactionResult{});
        return;
    }

    switch (applyResult.result.code())
    {
    case txINTERNAL_ERROR:
    case txBAD_AUTH_EXTRA:
    case txBAD_SEQ:
        return;
    default:
        break;
    }

    auto shouldApplyOk = applyResult.result.code() == txSUCCESS;
    auto applyOk = applyCheck(tx, app);
    REQUIRE(tx->getResult() == applyResult);
    REQUIRE(applyOk == shouldApplyOk);
};

TxSetResultMeta
closeLedgerOn(Application& app, uint32 ledgerSeq, int day, int month, int year,
              std::vector<TransactionFramePtr> const& txs)
{
    auto txSet = std::make_shared<TxSetFrame>(
        app.getLedgerManager().getLastClosedLedgerHeader().hash);

    for (auto const& tx : txs)
    {
        txSet->add(tx);
    }

    txSet->sortForHash();
    REQUIRE(txSet->checkValid(app));

    FoneroValue sv(txSet->getContentsHash(), getTestDate(day, month, year),
                    emptyUpgradeSteps, 0);
    LedgerCloseData ledgerData(ledgerSeq, txSet, sv);
    app.getLedgerManager().closeLedger(ledgerData);

    auto z1 = TransactionFrame::getTransactionHistoryResults(app.getDatabase(),
                                                             ledgerSeq);
    auto z2 =
        TransactionFrame::getTransactionFeeMeta(app.getDatabase(), ledgerSeq);

    REQUIRE(app.getLedgerManager().getLedgerNum() == (ledgerSeq + 1));

    TxSetResultMeta res;
    std::transform(
        z1.results.begin(), z1.results.end(), z2.begin(),
        std::back_inserter(res),
        [](TransactionResultPair const& r1, LedgerEntryChanges const& r2) {
            return std::make_pair(r1, r2);
        });

    return res;
}

SecretKey
getRoot(Hash const& networkID)
{
    return SecretKey::fromSeed(networkID);
}

SecretKey
getAccount(const char* n)
{
    // stretch seed to 32 bytes
    std::string seed(n);
    while (seed.size() < 32)
        seed += '.';
    return SecretKey::fromSeed(seed);
}

Signer
makeSigner(SecretKey key, int weight)
{
    return Signer{KeyUtils::convertKey<SignerKey>(key.getPublicKey()), weight};
}

AccountFrame::pointer
loadAccount(PublicKey const& k, Application& app, bool mustExist)
{
    auto res = AccountFrame::loadAccount(k, app.getDatabase());
    if (mustExist)
    {
        REQUIRE(res);
    }
    return res;
}

void
requireNoAccount(PublicKey const& k, Application& app)
{
    AccountFrame::pointer res = loadAccount(k, app, false);
    REQUIRE(!res);
}

OfferFrame::pointer
loadOffer(PublicKey const& k, uint64 offerID, Application& app, bool mustExist)
{
    OfferFrame::pointer res =
        OfferFrame::loadOffer(k, offerID, app.getDatabase());
    if (mustExist)
    {
        REQUIRE(res);
    }
    return res;
}

TrustFrame::pointer
loadTrustLine(SecretKey const& k, Asset const& asset, Application& app,
              bool mustExist)
{
    TrustFrame::pointer res =
        TrustFrame::loadTrustLine(k.getPublicKey(), asset, app.getDatabase());
    if (mustExist)
    {
        REQUIRE(res);
    }
    return res;
}

xdr::xvector<Signer, 20>
getAccountSigners(PublicKey const& k, Application& app)
{
    AccountFrame::pointer account;
    account = loadAccount(k, app);
    return account->getAccount().signers;
}

TransactionFramePtr
transactionFromOperations(Application& app, SecretKey const& from,
                          SequenceNumber seq, const std::vector<Operation>& ops)
{
    auto e = TransactionEnvelope{};
    e.tx.sourceAccount = from.getPublicKey();
    e.tx.fee = static_cast<uint32_t>(
        (ops.size() * app.getLedgerManager().getTxFee()) & UINT32_MAX);
    e.tx.seqNum = seq;
    std::copy(std::begin(ops), std::end(ops),
              std::back_inserter(e.tx.operations));

    auto res = TransactionFrame::makeTransactionFromWire(app.getNetworkID(), e);
    res->addSignature(from);
    return res;
}

Operation
changeTrust(Asset const& asset, int64_t limit)
{
    Operation op;

    op.body.type(CHANGE_TRUST);
    op.body.changeTrustOp().limit = limit;
    op.body.changeTrustOp().line = asset;

    return op;
}

Operation
allowTrust(PublicKey const& trustor, Asset const& asset, bool authorize)
{
    Operation op;

    op.body.type(ALLOW_TRUST);
    op.body.allowTrustOp().trustor = trustor;
    op.body.allowTrustOp().asset.type(ASSET_TYPE_CREDIT_ALPHANUM4);
    op.body.allowTrustOp().asset.assetCode4() = asset.alphaNum4().assetCode;
    op.body.allowTrustOp().authorize = authorize;

    return op;
}

Operation
createAccount(PublicKey const& dest, int64_t amount)
{
    Operation op;
    op.body.type(CREATE_ACCOUNT);
    op.body.createAccountOp().startingBalance = amount;
    op.body.createAccountOp().destination = dest;
    return op;
}

Operation
payment(PublicKey const& to, int64_t amount)
{
    Operation op;
    op.body.type(PAYMENT);
    op.body.paymentOp().amount = amount;
    op.body.paymentOp().destination = to;
    op.body.paymentOp().asset.type(ASSET_TYPE_NATIVE);
    return op;
}

Operation
payment(PublicKey const& to, Asset const& asset, int64_t amount)
{
    Operation op;
    op.body.type(PAYMENT);
    op.body.paymentOp().amount = amount;
    op.body.paymentOp().destination = to;
    op.body.paymentOp().asset = asset;
    return op;
}

TransactionFramePtr
createPaymentTx(Application& app, SecretKey const& from, PublicKey const& to,
                SequenceNumber seq, int64_t amount)
{
    return transactionFromOperations(app, from, seq, {payment(to, amount)});
}

TransactionFramePtr
createCreditPaymentTx(Application& app, SecretKey const& from,
                      PublicKey const& to, Asset const& asset,
                      SequenceNumber seq, int64_t amount)
{
    auto op = payment(to, asset, amount);
    return transactionFromOperations(app, from, seq, {op});
}

Asset
makeNativeAsset()
{
    Asset asset;
    asset.type(ASSET_TYPE_NATIVE);
    return asset;
}

Asset
makeInvalidAsset()
{
    Asset asset;
    asset.type(ASSET_TYPE_CREDIT_ALPHANUM4);
    return asset;
}

Asset
makeAsset(SecretKey const& issuer, std::string const& code)
{
    Asset asset;
    asset.type(ASSET_TYPE_CREDIT_ALPHANUM4);
    asset.alphaNum4().issuer = issuer.getPublicKey();
    strToAssetCode(asset.alphaNum4().assetCode, code);
    return asset;
}

Operation
pathPayment(PublicKey const& to, Asset const& sendCur, int64_t sendMax,
            Asset const& destCur, int64_t destAmount,
            std::vector<Asset> const& path)
{
    Operation op;
    op.body.type(PATH_PAYMENT);
    PathPaymentOp& ppop = op.body.pathPaymentOp();
    ppop.sendAsset = sendCur;
    ppop.sendMax = sendMax;
    ppop.destAsset = destCur;
    ppop.destAmount = destAmount;
    ppop.destination = to;
    std::copy(std::begin(path), std::end(path), std::back_inserter(ppop.path));

    return op;
}

Operation
createPassiveOffer(Asset const& selling, Asset const& buying,
                   Price const& price, int64_t amount)
{
    Operation op;
    op.body.type(CREATE_PASSIVE_OFFER);
    op.body.createPassiveOfferOp().amount = amount;
    op.body.createPassiveOfferOp().selling = selling;
    op.body.createPassiveOfferOp().buying = buying;
    op.body.createPassiveOfferOp().price = price;

    return op;
}

Operation
manageOffer(uint64 offerId, Asset const& selling, Asset const& buying,
            Price const& price, int64_t amount)
{
    Operation op;
    op.body.type(MANAGE_OFFER);
    op.body.manageOfferOp().amount = amount;
    op.body.manageOfferOp().selling = selling;
    op.body.manageOfferOp().buying = buying;
    op.body.manageOfferOp().offerID = offerId;
    op.body.manageOfferOp().price = price;

    return op;
}

static ManageOfferResult
applyCreateOfferHelper(Application& app, uint64 offerId,
                       SecretKey const& source, Asset const& selling,
                       Asset const& buying, Price const& price, int64_t amount,
                       SequenceNumber seq)
{
    auto lastGeneratedID =
        app.getLedgerManager().getCurrentLedgerHeader().idPool;
    auto expectedOfferID = lastGeneratedID + 1;
    if (offerId != 0)
    {
        expectedOfferID = offerId;
    }

    auto op = manageOffer(offerId, selling, buying, price, amount);
    auto tx = transactionFromOperations(app, source, seq, {op});

    try
    {
        applyTx(tx, app);
    }
    catch (...)
    {
        REQUIRE(app.getLedgerManager().getCurrentLedgerHeader().idPool ==
                lastGeneratedID);
        throw;
    }

    auto& results = tx->getResult().result.results();

    REQUIRE(results.size() == 1);

    auto& manageOfferResult = results[0].tr().manageOfferResult();

    OfferFrame::pointer offer;

    auto& offerResult = manageOfferResult.success().offer;

    switch (offerResult.effect())
    {
    case MANAGE_OFFER_CREATED:
    case MANAGE_OFFER_UPDATED:
    {
        offer = loadOffer(source.getPublicKey(), expectedOfferID, app, true);
        auto& offerEntry = offer->getOffer();
        REQUIRE(offerEntry == offerResult.offer());
        REQUIRE(offerEntry.price == price);
        REQUIRE(offerEntry.selling == selling);
        REQUIRE(offerEntry.buying == buying);
    }
    break;
    case MANAGE_OFFER_DELETED:
        REQUIRE(!loadOffer(source.getPublicKey(), expectedOfferID, app, false));
        break;
    default:
        abort();
    }

    return manageOfferResult;
}

uint64_t
applyManageOffer(Application& app, uint64 offerId, SecretKey const& source,
                 Asset const& selling, Asset const& buying, Price const& price,
                 int64_t amount, SequenceNumber seq,
                 ManageOfferEffect expectedEffect)
{
    ManageOfferResult const& createOfferRes = applyCreateOfferHelper(
        app, offerId, source, selling, buying, price, amount, seq);

    auto& success = createOfferRes.success().offer;
    REQUIRE(success.effect() == expectedEffect);
    return success.effect() != MANAGE_OFFER_DELETED ? success.offer().offerID
                                                    : 0;
}

uint64_t
applyCreatePassiveOffer(Application& app, SecretKey const& source,
                        Asset const& selling, Asset const& buying,
                        Price const& price, int64_t amount, SequenceNumber seq,
                        ManageOfferEffect expectedEffect)
{
    auto lastGeneratedID =
        app.getLedgerManager().getCurrentLedgerHeader().idPool;
    auto expectedOfferID = lastGeneratedID + 1;

    auto op = createPassiveOffer(selling, buying, price, amount);
    auto tx = transactionFromOperations(app, source, seq, {op});

    try
    {
        applyTx(tx, app);
    }
    catch (...)
    {
        REQUIRE(app.getLedgerManager().getCurrentLedgerHeader().idPool ==
                lastGeneratedID);
        throw;
    }

    auto& results = tx->getResult().result.results();

    REQUIRE(results.size() == 1);

    auto& createPassiveOfferResult = results[0].tr().manageOfferResult();

    if (createPassiveOfferResult.code() == MANAGE_OFFER_SUCCESS)
    {
        OfferFrame::pointer offer;

        auto& offerResult = createPassiveOfferResult.success().offer;

        switch (offerResult.effect())
        {
        case MANAGE_OFFER_CREATED:
        case MANAGE_OFFER_UPDATED:
        {
            offer =
                loadOffer(source.getPublicKey(), expectedOfferID, app, true);
            auto& offerEntry = offer->getOffer();
            REQUIRE(offerEntry == offerResult.offer());
            REQUIRE(offerEntry.price == price);
            REQUIRE(offerEntry.selling == selling);
            REQUIRE(offerEntry.buying == buying);
            REQUIRE((offerEntry.flags & PASSIVE_FLAG) != 0);
        }
        break;
        case MANAGE_OFFER_DELETED:
            REQUIRE(
                !loadOffer(source.getPublicKey(), expectedOfferID, app, false));
            break;
        default:
            abort();
        }
    }

    auto& success = createPassiveOfferResult.success().offer;

    REQUIRE(success.effect() == expectedEffect);

    return success.effect() == MANAGE_OFFER_CREATED ? success.offer().offerID
                                                    : 0;
}

SetOptionsArguments
operator|(SetOptionsArguments const& x, SetOptionsArguments const& y)
{
    auto result = SetOptionsArguments{};
    result.masterWeight = y.masterWeight ? y.masterWeight : x.masterWeight;
    result.lowThreshold = y.lowThreshold ? y.lowThreshold : x.lowThreshold;
    result.medThreshold = y.medThreshold ? y.medThreshold : x.medThreshold;
    result.highThreshold = y.highThreshold ? y.highThreshold : x.highThreshold;
    result.signer = y.signer ? y.signer : x.signer;
    result.setFlags = y.setFlags ? y.setFlags : x.setFlags;
    result.clearFlags = y.clearFlags ? y.clearFlags : x.clearFlags;
    result.inflationDest = y.inflationDest ? y.inflationDest : x.inflationDest;
    result.homeDomain = y.homeDomain ? y.homeDomain : x.homeDomain;
    return result;
}

Operation
setOptions(SetOptionsArguments const& arguments)
{
    Operation op;
    op.body.type(SET_OPTIONS);

    SetOptionsOp& setOp = op.body.setOptionsOp();

    if (arguments.inflationDest)
    {
        setOp.inflationDest.activate() = *arguments.inflationDest;
    }

    if (arguments.setFlags)
    {
        setOp.setFlags.activate() = *arguments.setFlags;
    }

    if (arguments.clearFlags)
    {
        setOp.clearFlags.activate() = *arguments.clearFlags;
    }

    if (arguments.masterWeight)
    {
        setOp.masterWeight.activate() = *arguments.masterWeight;
    }
    if (arguments.lowThreshold)
    {
        setOp.lowThreshold.activate() = *arguments.lowThreshold;
    }
    if (arguments.medThreshold)
    {
        setOp.medThreshold.activate() = *arguments.medThreshold;
    }
    if (arguments.highThreshold)
    {
        setOp.highThreshold.activate() = *arguments.highThreshold;
    }

    if (arguments.signer)
    {
        setOp.signer.activate() = *arguments.signer;
    }

    if (arguments.homeDomain)
    {
        setOp.homeDomain.activate() = *arguments.homeDomain;
    }

    return op;
}

SetOptionsArguments
setMasterWeight(int master)
{
    SetOptionsArguments result;
    result.masterWeight = make_optional<int>(master);
    return result;
}

SetOptionsArguments
setLowThreshold(int low)
{
    SetOptionsArguments result;
    result.lowThreshold = make_optional<int>(low);
    return result;
}

SetOptionsArguments
setMedThreshold(int med)
{
    SetOptionsArguments result;
    result.medThreshold = make_optional<int>(med);
    return result;
}

SetOptionsArguments
setHighThreshold(int high)
{
    SetOptionsArguments result;
    result.highThreshold = make_optional<int>(high);
    return result;
}

SetOptionsArguments
setSigner(Signer signer)
{
    SetOptionsArguments result;
    result.signer = make_optional<Signer>(signer);
    return result;
}

SetOptionsArguments
setFlags(uint32_t setFlags)
{
    SetOptionsArguments result;
    result.setFlags = make_optional<uint32_t>(setFlags);
    return result;
}

SetOptionsArguments
clearFlags(uint32_t clearFlags)
{
    SetOptionsArguments result;
    result.clearFlags = make_optional<uint32_t>(clearFlags);
    return result;
}

SetOptionsArguments
setInflationDestination(AccountID inflationDest)
{
    SetOptionsArguments result;
    result.inflationDest = make_optional<AccountID>(inflationDest);
    return result;
}

SetOptionsArguments
setHomeDomain(std::string const& homeDomain)
{
    SetOptionsArguments result;
    result.homeDomain = make_optional<std::string>(homeDomain);
    return result;
}

Operation
inflation()
{
    Operation op;
    op.body.type(INFLATION);

    return op;
}

Operation
accountMerge(PublicKey const& dest)
{
    Operation op;
    op.body.type(ACCOUNT_MERGE);
    op.body.destination() = dest;
    return op;
}

Operation
manageData(std::string const& name, DataValue* value)
{
    Operation op;
    op.body.type(MANAGE_DATA);
    op.body.manageDataOp().dataName = name;
    if (value)
        op.body.manageDataOp().dataValue.activate() = *value;

    return op;
}

Operation
bumpSequence(SequenceNumber to)
{
    Operation op;
    op.body.type(BUMP_SEQUENCE);
    op.body.bumpSequenceOp().bumpTo = to;
    return op;
}

OperationFrame const&
getFirstOperationFrame(TransactionFrame const& tx)
{
    return *(tx.getOperations()[0]);
}

OperationResult const&
getFirstResult(TransactionFrame const& tx)
{
    return getFirstOperationFrame(tx).getResult();
}

OperationResultCode
getFirstResultCode(TransactionFrame const& tx)
{
    return getFirstOperationFrame(tx).getResultCode();
}

void
checkTx(int index, TxSetResultMeta& r, TransactionResultCode expected)
{
    REQUIRE(r[index].first.result.result.code() == expected);
};

void
checkTx(int index, TxSetResultMeta& r, TransactionResultCode expected,
        OperationResultCode code)
{
    checkTx(index, r, expected);
    REQUIRE(r[index].first.result.result.results()[0].code() == code);
};
}
}
