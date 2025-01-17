// Copyright (c) 2019-2021 Chaintope Inc.
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#include <qt/test/wallettests.h>
#include <qt/test/util.h>

#include <interfaces/node.h>
#include <qt/tapyrusamountfield.h>
#include <base58.h>
#include <qt/callback.h>
#include <qt/optionsmodel.h>
#include <qt/platformstyle.h>
#include <qt/qvalidatedlineedit.h>
#include <qt/sendcoinsdialog.h>
#include <qt/sendcoinsentry.h>
#include <qt/transactiontablemodel.h>
#include <qt/transactionview.h>
#include <qt/walletmodel.h>
#include <key_io.h>
#include <test/test_tapyrus.h>
#include <validation.h>
#include <wallet/wallet.h>
#include <qt/overviewpage.h>
#include <qt/receivecoinsdialog.h>
#include <qt/recentrequeststablemodel.h>
#include <qt/receiverequestdialog.h>

#include <memory>

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>
#include <QTextEdit>
#include <QListView>
#include <QDialogButtonBox>
#include <QTemporaryFile>

namespace
{
//! Press "Yes" or "Cancel" buttons in modal send confirmation dialog.
void ConfirmSend(QString* text = nullptr, bool cancel = false)
{
    QTimer::singleShot(0, makeCallback([text, cancel](Callback* callback) {
        for (QWidget* widget : QApplication::topLevelWidgets()) {
            if (widget->inherits("SendConfirmationDialog")) {
                SendConfirmationDialog* dialog = qobject_cast<SendConfirmationDialog*>(widget);
                if (text) *text = dialog->text();
                QAbstractButton* button = dialog->button(cancel ? QMessageBox::Cancel : QMessageBox::Yes);
                button->setEnabled(true);
                button->click();
            }
        }
        delete callback;
    }), SLOT(call()));
}

//! Send coins to address and return txid.
uint256 SendCoins(CWallet& wallet, SendCoinsDialog& sendCoinsDialog, const CTxDestination& address, CAmount amount, bool rbf)
{
    QVBoxLayout* entries = sendCoinsDialog.findChild<QVBoxLayout*>("entries");
    SendCoinsEntry* entry = qobject_cast<SendCoinsEntry*>(entries->itemAt(0)->widget());
    entry->findChild<QValidatedLineEdit*>("payTo")->setText(QString::fromStdString(EncodeDestination(address)));
    entry->findChild<BitcoinAmountField*>("payAmount")->setValue(amount);
    sendCoinsDialog.findChild<QFrame*>("frameFee")
        ->findChild<QFrame*>("frameFeeSelection")
        ->findChild<QCheckBox*>("optInRBF")
        ->setCheckState(rbf ? Qt::Checked : Qt::Unchecked);
    uint256 txid;
    boost::signals2::scoped_connection c(wallet.NotifyTransactionChanged.connect([&txid](CWallet*, const uint256& hash, ChangeType status) {
        if (status == CT_NEW) txid = hash;
    }));
    ConfirmSend();
    QMetaObject::invokeMethod(&sendCoinsDialog, "on_sendButton_clicked");
    return txid;
}

//! Find index of txid in transaction list.
QModelIndex FindTx(const QAbstractItemModel& model, const uint256& txid)
{
    QString hash = QString::fromStdString(txid.ToString());
    int rows = model.rowCount({});
    for (int row = 0; row < rows; ++row) {
        QModelIndex index = model.index(row, 0, {});
        if (model.data(index, TransactionTableModel::TxHashRole) == hash) {
            return index;
        }
    }
    return {};
}

//! Invoke bumpfee on txid and check results.
void BumpFee(TransactionView& view, const uint256& txid, bool expectDisabled, std::string expectError, bool cancel)
{
    QTableView* table = view.findChild<QTableView*>("transactionView");
    QModelIndex index = FindTx(*table->selectionModel()->model(), txid);
    QVERIFY2(index.isValid(), "Could not find BumpFee txid");

    // Select row in table, invoke context menu, and make sure bumpfee action is
    // enabled or disabled as expected.
    QAction* action = view.findChild<QAction*>("bumpFeeAction");
    table->selectionModel()->select(index, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    action->setEnabled(expectDisabled);
    table->customContextMenuRequested({});
    QCOMPARE(action->isEnabled(), !expectDisabled);

    action->setEnabled(true);
    QString text;
    if (expectError.empty()) {
        ConfirmSend(&text, cancel);
    } else {
        ConfirmMessage(&text);
    }
    action->trigger();
    QVERIFY(text.indexOf(QString::fromStdString(expectError)) != -1);
}

//! Simple qt wallet tests.
//
// Test widgets can be debugged interactively calling show() on them and
// manually running the event loop, e.g.:
//
//     sendCoinsDialog.show();
//     QEventLoop().exec();
//
// This also requires overriding the default minimal Qt platform:
//
//     src/qt/test/test_tapyrus_qt -platform xcb      # Linux
//     src/qt/test/test_tapyrus_qt -platform windows  # Windows
//     src/qt/test/test_tapyrus_qt -platform cocoa    # macOS
void TestGUI()
{
    // Set up wallet and chain with 105 blocks (5 mature blocks for spending).
    TestChainSetup test;
    for (int i = 0; i < 5; ++i) {
        test.CreateAndProcessBlock({}, GetScriptForRawPubKey(test.coinbaseKey.GetPubKey()));
    }
    std::shared_ptr<CWallet> wallet = std::make_shared<CWallet>("mock", WalletDatabase::CreateMock());
    bool firstRun;
    wallet->LoadWallet(firstRun);
    {
        LOCK(wallet->cs_wallet);
        wallet->SetAddressBook(GetDestinationForKey(test.coinbaseKey.GetPubKey(), wallet->m_default_address_type), "", "receive");
        wallet->AddKeyPubKey(test.coinbaseKey, test.coinbaseKey.GetPubKey());
    }
    {
        LOCK(cs_main);
        WalletRescanReserver reserver(wallet.get());
        reserver.reserve();
        wallet->ScanForWalletTransactions(chainActive.Genesis(), nullptr, reserver, true);
    }
    wallet->SetBroadcastTransactions(true);

    // Create widgets for sending coins and listing transactions.
    std::unique_ptr<const PlatformStyle> platformStyle(PlatformStyle::instantiate("other"));
    SendCoinsDialog sendCoinsDialog(platformStyle.get());
    TransactionView transactionView(platformStyle.get());
    auto node = interfaces::MakeNode();
    OptionsModel optionsModel(*node);
    AddWallet(wallet);
    WalletModel walletModel(std::move(node->getWallets().back()), *node, platformStyle.get(), &optionsModel);
    RemoveWallet(wallet);
    sendCoinsDialog.setModel(&walletModel);
    transactionView.setModel(&walletModel);

    // Send two transactions, and verify they are added to transaction list.
    TransactionTableModel* transactionTableModel = walletModel.getTransactionTableModel();
    QCOMPARE(transactionTableModel->rowCount({}), 10);
    uint256 txid1 = SendCoins(*wallet.get(), sendCoinsDialog, CKeyID(), 5 * COIN, false /* rbf */);
    uint256 txid2 = SendCoins(*wallet.get(), sendCoinsDialog, CKeyID(), 10 * COIN, true /* rbf */);
    QCOMPARE(transactionTableModel->rowCount({}), 12);
    QVERIFY(FindTx(*transactionTableModel, txid1).isValid());
    QVERIFY(FindTx(*transactionTableModel, txid2).isValid());

    // Call bumpfee. Test disabled, canceled, enabled, then failing cases.
    BumpFee(transactionView, txid1, true /* expect disabled */, "not BIP 125 replaceable" /* expected error */, false /* cancel */);
    BumpFee(transactionView, txid2, false /* expect disabled */, {} /* expected error */, true /* cancel */);
    BumpFee(transactionView, txid2, false /* expect disabled */, {} /* expected error */, false /* cancel */);
    BumpFee(transactionView, txid2, true /* expect disabled */, "already bumped" /* expected error */, false /* cancel */);

    // Check current balance on OverviewPage
    OverviewPage overviewPage(platformStyle.get());
    overviewPage.setWalletModel(&walletModel);
    QLabel* balanceLabel = overviewPage.findChild<QLabel*>("labelBalance");
    QString balanceText = balanceLabel->text();
    int unit = walletModel.getOptionsModel()->getDisplayUnit();
    CAmount balance = walletModel.wallet().getBalance();
    QString balanceComparison = BitcoinUnits::formatWithUnit(unit, balance, false, BitcoinUnits::separatorAlways);
    QCOMPARE(balanceText, balanceComparison);

    // Check Request Payment button
    ReceiveCoinsDialog receiveCoinsDialog(platformStyle.get());
    receiveCoinsDialog.setModel(&walletModel);
    RecentRequestsTableModel* requestTableModel = walletModel.getRecentRequestsTableModel();

    // Label input
    QLineEdit* labelInput = receiveCoinsDialog.findChild<QLineEdit*>("reqLabel");
    labelInput->setText("TEST_LABEL_1");

    // Amount input
    BitcoinAmountField* amountInput = receiveCoinsDialog.findChild<BitcoinAmountField*>("reqAmount");
    amountInput->setValue(1);

    // Message input
    QLineEdit* messageInput = receiveCoinsDialog.findChild<QLineEdit*>("reqMessage");
    messageInput->setText("TEST_MESSAGE_1");
    int initialRowCount = requestTableModel->rowCount({});
    QPushButton* requestPaymentButton = receiveCoinsDialog.findChild<QPushButton*>("receiveButton");
    requestPaymentButton->click();
    for (QWidget* widget : QApplication::topLevelWidgets()) {
        if (widget->inherits("ReceiveRequestDialog")) {
            ReceiveRequestDialog* receiveRequestDialog = qobject_cast<ReceiveRequestDialog*>(widget);
            QTextEdit* rlist = receiveRequestDialog->QObject::findChild<QTextEdit*>("outUri");
            QString paymentText = rlist->toPlainText();
            QStringList paymentTextList = paymentText.split('\n');
            QCOMPARE(paymentTextList.at(0), QString("Payment information"));
            QVERIFY(paymentTextList.at(1).indexOf(QString("URI: tapyrus:")) != -1);
            QVERIFY(paymentTextList.at(2).indexOf(QString("Address:")) != -1);
            QCOMPARE(paymentTextList.at(3), QString("Amount: 0.00000001 ") + QString::fromStdString(CURRENCY_UNIT));
            QCOMPARE(paymentTextList.at(4), QString("Label: TEST_LABEL_1"));
            QCOMPARE(paymentTextList.at(5), QString("Message: TEST_MESSAGE_1"));
        }
    }

    // Clear button
    QPushButton* clearButton = receiveCoinsDialog.findChild<QPushButton*>("clearButton");
    clearButton->click();
    QCOMPARE(labelInput->text(), QString(""));
    QCOMPARE(amountInput->value(), CAmount(0));
    QCOMPARE(messageInput->text(), QString(""));

    // Check addition to history
    int currentRowCount = requestTableModel->rowCount({});
    QCOMPARE(currentRowCount, initialRowCount+1);

    // Check Remove button
    QTableView* table = receiveCoinsDialog.findChild<QTableView*>("recentRequestsView");
    table->selectRow(currentRowCount-1);
    QPushButton* removeRequestButton = receiveCoinsDialog.findChild<QPushButton*>("removeRequestButton");
    removeRequestButton->click();
    QCOMPARE(requestTableModel->rowCount({}), currentRowCount-1);
}

} // namespace

class SendCoinsRecipient;

void RecipientCatcher::getRecipient(const SendCoinsRecipient& r)
{
    recipient = r;
}

static SendCoinsRecipient handleRequest(PaymentServer* server, std::vector<unsigned char>& data)
{
    RecipientCatcher sigCatcher;
    QObject::connect(server, SIGNAL(receivedPaymentRequest(SendCoinsRecipient)),
        &sigCatcher, SLOT(getRecipient(SendCoinsRecipient)));

    // Write data to a temp file:
    QTemporaryFile f;
    f.open();
    f.write((const char*)data.data(), data.size());
    f.close();

    // Create a QObject, install event filter from PaymentServer
    // and send a file open event to the object
    QObject object;
    object.installEventFilter(server);
    QFileOpenEvent event(f.fileName());
    // If sending the event fails, this will cause sigCatcher to be empty,
    // which will lead to a test failure anyway.
    QCoreApplication::sendEvent(&object, &event);

    QObject::disconnect(server, SIGNAL(receivedPaymentRequest(SendCoinsRecipient)),
        &sigCatcher, SLOT(getRecipient(SendCoinsRecipient)));

    // Return results from sigCatcher
    return sigCatcher.recipient;
}

void paymentServerTest()
{
    SelectParams(TAPYRUS_OP_MODE::PROD);
    auto node = interfaces::MakeNode();
    OptionsModel optionsModel(*node);
    PaymentServer* server = new PaymentServer(nullptr, false);
    server->setOptionsModel(&optionsModel);
    server->uiReady();

    std::vector<unsigned char> data;
    SendCoinsRecipient r;
    QString merchant;

    // Now feed PaymentRequests to server, and observe signals it produces

    // This payment request validates directly against the
    // caCert1 certificate authority:
    data = DecodeBase64("Egt4NTA5K3NoYTI1NhrxAwruAzCCAeowggFToAMCAQICAQEwDQYJKoZIhvcNAQEL\
    BQAwITEfMB0GA1UEAxMWUGF5bWVudFJlcXVlc3QgVGVzdCBDQTAeFw0xMjEyMTAx\
    NjM3MjRaFw0yMjEyMDgxNjM3MjRaMEMxGTAXBgNVBAMMEHRlc3RtZXJjaGFudC5v\
    cmcxJjAkBgNVBAoMHVBheW1lbnQgUmVxdWVzdCBUZXN0IE1lcmNoYW50MIGfMA0G\
    CSqGSIb3DQEBAQUAA4GNADCBiQKBgQDHkMy8W1u6HsWlSqdWTmMKf54gICxNfxbY\
    +rcMtAftr62hCYx2d2QiSRd1pCUzmo12IiSX3WxSHwaTnT3MFD6jRx6+zM6XdGar\
    I2zpYle11ANzu4gAthN17uRQHV2O5QxVtzNaMdKeJLXT2L9tfEdyL++9ZUqoQmdA\
    YG9ix330hQIDAQABoxAwDjAMBgNVHRMEBTADAQH/MA0GCSqGSIb3DQEBCwUAA4GB\
    AIkyO99KC68bi9PFRyQQ7nvn5GlQEb3Ca1bRG5+AKN9N5vc8rZ9G2hejtM8wEXni\
    eGBP+chVMsbTPEHKLrwREn7IvcyCcbAStaklPC3w0B/2idQSHskb6P3X13OR2bTH\
    a2+6wuhsOZRUrVNr24rM95DKx/eCC6JN1VW+qRPU6fqzIjQSHwiw2wYSGXapFJVg\
    igPI+6XpExtNLO/i1WFV8ZmoiKwYsuHFiwUqC1VuaXRUZXN0T25lKoABS0j59iMU\
    Uc9MdIfwsO1BskIET0eJSGNZ7eXb9N62u+qf831PMpEHkmlGpk8rHy92nPcgua/U\
    Yt8oZMn3QaTZ5A6HjJbc3A73eLylp1a0SwCl+KDMEvDQhqMn1jAVu2v92AH3uB7n\
    SiWVbw0tX/68iSQEGGfh9n6ee/8Myb3ICdw=");
    QVERIFY(data.size());
    r = handleRequest(server, data);
    QCOMPARE(r.amount, (int64_t)0);
    QCOMPARE(r.label, QString(""));
    QCOMPARE(r.message, QString(""));
    QCOMPARE(r.address, QString(""));
    QCOMPARE(r.sPaymentRequest.c_str(), "");
}

void WalletTests::walletTests()
{
    paymentServerTest();
    TestGUI();
}
