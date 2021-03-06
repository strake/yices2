(set-info :source |fuzzsmt|)
(set-info :smt-lib-version 2.0)
(set-info :category "random")
(set-info :status unknown)
(set-logic QF_BV)
(declare-fun v0 () (_ BitVec 14))
(declare-fun v1 () (_ BitVec 6))
(assert (let ((e2(_ bv2 2)))
(let ((e3 (bvashr v0 ((_ sign_extend 8) v1))))
(let ((e4 (ite (= (_ bv1 1) ((_ extract 2 2) v1)) ((_ sign_extend 8) v1) e3)))
(let ((e5 (ite (bvsge e4 ((_ zero_extend 8) v1)) (_ bv1 1) (_ bv0 1))))
(let ((e6 ((_ rotate_left 0) e5)))
(let ((e7 (bvudiv ((_ zero_extend 12) e2) e3)))
(let ((e8 (bvslt v0 v0)))
(let ((e9 (bvslt e7 e7)))
(let ((e10 (bvule e2 e2)))
(let ((e11 (bvslt ((_ zero_extend 12) e2) e3)))
(let ((e12 (bvsle e3 v0)))
(let ((e13 (bvugt ((_ sign_extend 12) e2) e4)))
(let ((e14 (bvslt ((_ sign_extend 12) e2) e3)))
(let ((e15 (bvule v0 ((_ zero_extend 8) v1))))
(let ((e16 (bvult v1 ((_ sign_extend 5) e6))))
(let ((e17 (bvslt v1 ((_ sign_extend 5) e6))))
(let ((e18 (bvuge e7 ((_ sign_extend 13) e5))))
(let ((e19 (ite e16 e17 e8)))
(let ((e20 (not e10)))
(let ((e21 (= e11 e15)))
(let ((e22 (=> e12 e19)))
(let ((e23 (not e18)))
(let ((e24 (=> e22 e14)))
(let ((e25 (ite e24 e23 e20)))
(let ((e26 (and e25 e25)))
(let ((e27 (or e26 e26)))
(let ((e28 (xor e27 e13)))
(let ((e29 (= e21 e28)))
(let ((e30 (xor e29 e9)))
(let ((e31 (and e30 (not (= e3 (_ bv0 14))))))
e31
)))))))))))))))))))))))))))))))

(check-sat)
