#include "rj/econ/ledger.hpp"
#include "rj_test.hpp"

using namespace rj::econ;

RJ_TEST(ledger_conserves_money) {
  Ledger L;
  const AccountId alice = L.open(AccountKind::Person, "alice");
  const AccountId shop = L.open(AccountKind::Business, "shop");
  RJ_CHECK(L.endow(alice, 50000, 0) == TxResult::Ok);
  RJ_CHECK_EQ(L.sumAll(), 0);
  RJ_CHECK_EQ(L.moneyInWorld(), 50000);
  RJ_CHECK(L.transfer(alice, shop, 1200, TxCategory::Purchase, 1) == TxResult::Ok);
  RJ_CHECK(L.transfer(alice, shop, 999999, TxCategory::Purchase, 2) == TxResult::InsufficientFunds);
  RJ_CHECK(L.transfer(alice, shop, 0, TxCategory::Purchase, 3) == TxResult::InvalidAmount);
  RJ_CHECK(L.transfer(alice, 9999, 10, TxCategory::Purchase, 4) == TxResult::UnknownAccount);
  RJ_CHECK_EQ(L.balance(alice), 48800);
  RJ_CHECK_EQ(L.balance(shop), 1200);
  RJ_CHECK_EQ(L.sumAll(), 0);
  RJ_CHECK_EQ(L.moneyInWorld(), 50000);
  RJ_CHECK_EQ(L.journal().size(), 2u);
}

RJ_TEST(recurring_payments_and_arrears) {
  Ledger L;
  const AccountId tenant = L.open(AccountKind::Person, "tenant");
  const AccountId landlord = L.open(AccountKind::Business, "landlord");
  const AccountId power = L.open(AccountKind::Utility, "power");
  L.endow(tenant, 100000, 0);
  RecurringScheduler rs;
  rs.add({tenant, landlord, 80000, TxCategory::Rent, 0, 30 * 86400, "rent"});
  rs.add({tenant, power, 6000, TxCategory::Electricity, 0, 30 * 86400, "electricity"});
  RJ_CHECK_EQ(rs.run(L, 0), 0);
  RJ_CHECK_EQ(L.balance(tenant), 14000);
  // Next month: cannot pay rent -> arrears recorded; electricity still paid.
  RJ_CHECK_EQ(rs.run(L, 30 * 86400), 1);
  RJ_CHECK_EQ(rs.items()[0].missed, 1);
  RJ_CHECK_EQ(L.balance(tenant), 8000);
  RJ_CHECK_EQ(L.sumAll(), 0);
}

RJ_TEST(payroll_withholding_goes_to_government) {
  Ledger L;
  const AccountId company = L.open(AccountKind::Business, "company");
  const AccountId worker = L.open(AccountKind::Person, "worker");
  const AccountId gov = L.open(AccountKind::Government, "gov", 1'000'000'000);
  L.endow(company, 1'000'000, 0);
  RJ_CHECK(payWage(L, company, worker, gov, 300000, 0.2, 1) == TxResult::Ok);
  RJ_CHECK_EQ(L.balance(worker), 240000);
  RJ_CHECK_EQ(L.balance(gov), 60000);
  RJ_CHECK_EQ(L.balance(company), 700000);
  RJ_CHECK_EQ(L.sumAll(), 0);
}
